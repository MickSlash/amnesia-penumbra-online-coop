// SPDX-License-Identifier: GPL-3.0-or-later
#include "LuxNetwork.h"
#include "LuxMainMenu.h"
#include "LuxSteam.h"
#include "LuxPlayer.h"
#include "LuxMapHandler.h"
#include "LuxMap.h"
#include "LuxInputHandler.h"
#include "LuxPlayerState.h"
#include "LuxPlayerHelpers.h"
#include "LuxPlayerHands.h"
#include "LuxHandObject.h"
#include "LuxPlayerAvatar.h"
#include "LuxProp.h"
#include "LuxArea.h"
#include "LuxProp_Lamp.h"
#include "LuxProp_EmotionStone.h"
#include "LuxMoveState_Normal.h"
#include "LuxProp_SwingDoor.h"
#include "LuxProp_Lever.h"
#include "LuxProp_Wheel.h"
#include "LuxEnemy.h"
#include "LuxInventory.h"
#include "LuxProp_Item.h"
#include "LuxSaveHandler.h"
#include "LuxEffectHandler.h"
#include "LuxJournal.h"
#include "LuxMessageHandler.h"
#include "LuxHintHandler.h"
#include "LuxMusicHandler.h"
#include "LuxHelpFuncs.h"
#include "LuxSavedGameTypes.h"
#include "LuxSavedGame.h"
#include "impl/tinyXML/tinyxml.h"
#include "resources/BinaryBuffer.h"
#include "../../../HPL2/dependencies/sources/zlib/zlib.h"
#include <cstring>
#include "resources/SoundEntityManager.h"
#include "resources/ParticleManager.h"
#include "network/ScriptPreloads.h"
#include "sound/SoundEntityData.h"
#include "sound/SoundChannel.h"
#include "scene/Node3D.h"
#include <cstdio>
#include <ctime>
#include "impl/ImGuiConsole.h"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <future>
#include <thread>
#include <limits>
#include <stdexcept>
#include <atomic>
namespace {
bool TestFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string(value) == "1";
}
std::string MediaAssetName(std::string file) {
    std::replace(file.begin(), file.end(), '\\', '/');
    // Original .ent files can retain the author's E:/Projects/... pathname.
    // HPL already loaded these through its filename fallback. Transmit that
    // logical resource name rather than an unusable or absolute wire path.
    if(file.find(':') != std::string::npos || file.find("..") != std::string::npos || (!file.empty() && file[0] == '/'))
        file = cString::GetFileName(file);
    return file;
}
bool Port(const std::string& text, uint16_t& value) {
    if (text.empty() || text.size() > 5) return false;
    unsigned number = 0;
    for (char c : text) { if (c < '0' || c > '9') return false; number = number * 10 + (c - '0'); }
    if (!number || number > 65535) return false;
    value = uint16_t(number); return true;
}
// Local wall-clock time, HH:MM:SS.mmm. The [t=...] counters are per-PC
// monotonic clocks; this is what lines up with the partner's hpl.log and with
// tools/NetworkProbe.ps1 (PC clocks still differ by their own offset).
static std::string LocalClock() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const int millis = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local = {};
    localtime_s(&local, &seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, millis);
    return text;
}
void NetLog(const std::string& line) {
    std::string text = line;
    if(line.compare(0, 10, "[NET/DIAG]") == 0 || line.compare(0, 16, "[NET/CONNECTION]") == 0 || line.compare(0, 14, "[NET/RECOVERY]") == 0)
        text += " @" + LocalClock();
    Log("%s\n", text.c_str());
    cImGuiConsole::AddLog("%s", text.c_str());
}
online::Transform PackTransform(const cMatrixf& matrix) {
    online::Transform result;
    for(int r = 0; r < 3; ++r) for(int c = 0; c < 4; ++c) {
        const float scale = c == 3 ? 1000.0f : 10000.0f;
        result[r * 4 + c] = int32_t(std::round(matrix.m[r][c] * scale));
    }
    return result;
}
cMatrixf UnpackTransform(const online::Transform& values) {
    cMatrixf result = cMatrixf::Identity;
    for(int r = 0; r < 3; ++r) for(int c = 0; c < 4; ++c)
        result.m[r][c] = values[r * 4 + c] / (c == 3 ? 1000.0f : 10000.0f);
    return result;
}
void EnsureMediaIDs(cWorld* world, int& next) {
    auto sounds = world->GetSoundEntityIterator();
    auto particles = world->GetParticleSystemIterator();
    while(sounds.HasNext()) next = std::max(next, sounds.Next()->GetUniqueID() + 1);
    while(particles.HasNext()) next = std::max(next, particles.Next()->GetUniqueID() + 1);
    sounds = world->GetSoundEntityIterator(); particles = world->GetParticleSystemIterator();
    while(sounds.HasNext()) { auto* object = sounds.Next(); if(object->GetUniqueID() < 0) object->SetUniqueID(next++); }
    while(particles.HasNext()) { auto* object = particles.Next(); if(object->GetUniqueID() < 0) object->SetUniqueID(next++); }
}
uint64_t MapFingerprint(const std::string& name) {
    uint64_t hash = 14695981039346656037ull;
    auto* searcher = gpBase->mpEngine->GetResources()->GetFileSearcher();
    for(int kind = 0; kind < 2; ++kind) {
        const tString file = kind ? cString::SetFileExt(name, "hps") : name;
        tWString path = searcher->GetFilePath(file);
        if(path.empty()) { if(!kind) return 0; hash ^= 0xff; hash *= 1099511628211ull; continue; }
        FILE* input = cPlatform::OpenFile(path, _W("rb"));
        if(!input) return 0;
        hash ^= uint8_t(kind); hash *= 1099511628211ull;
        uint8_t buffer[65536]; size_t count;
        while((count = std::fread(buffer, 1, sizeof(buffer), input)) != 0)
            for(size_t i = 0; i < count; ++i) { hash ^= buffer[i]; hash *= 1099511628211ull; }
        const bool error = std::ferror(input) != 0; std::fclose(input);
        if(error) return 0;
    }
    return hash;
}
void PreloadReplicaAssets(const std::string& map) {
    auto* resources = gpBase->mpEngine->GetResources();
    const auto path = resources->GetFileSearcher()->GetFilePath(cString::SetFileExt(map,"hps"));
    if(path.empty()) return;
    FILE* input = cPlatform::OpenFile(path,_W("rb")); if(!input) return;
    std::string script; char buffer[8192]; size_t count;
    while((count = std::fread(buffer,1,sizeof(buffer),input)) != 0 && script.size() <= 2*1024*1024) script.append(buffer,count);
    const bool error = std::ferror(input) != 0; std::fclose(input); if(error) return;
    const auto start = std::chrono::steady_clock::now(); unsigned sounds = 0, particles = 0;
    for(const auto& asset : online::ScriptPreloads(script)) {
        if(asset.particle) { resources->GetParticleManager()->Preload(asset.file); ++particles; }
        else { resources->GetSoundEntityManager()->Preload(asset.file); ++sounds; }
    }
    NetLog("[NET/MAP] Replica assets preloaded sounds=" + std::to_string(sounds) + " particles=" + std::to_string(particles) +
        " load_us=" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count()));
}
}
cLuxNetwork::cLuxNetwork() : iLuxUpdateable("LuxNetwork"), mSession(NetLog) {}
cLuxNetwork::~cLuxNetwork() {
    for (const char* name : {"net_host", "net_join", "net_disconnect", "net_status", "net_ping"})
        cImGuiConsole::UnregisterCommand(name);
}
void cLuxNetwork::OnStart() {
    const auto lastRole = gpBase->mpMainConfig->GetString("Online","LastRole","");
    mLastRole = lastRole == "host" ? online::Role::Host : lastRole == "client" ? online::Role::Client : online::Role::Offline;
    mClearImage = gpBase->mpMainConfig->GetBool("Online","ClearImage",true);
    const tString savedIdentity = gpBase->mpMainConfig->GetString("Online","ResumeIdentity","");
    char* identityEnd = NULL; const uint64_t identity = std::strtoull(savedIdentity.c_str(),&identityEnd,10);
    if(!savedIdentity.empty() && identityEnd && !*identityEnd && identity) mSession.SetLocalIdentity(identity);
    else {
        gpBase->mpMainConfig->SetString("Online","ResumeIdentity",std::to_string(mSession.LocalIdentity()));
        if(gpBase->mbSaveConfigAtExit) gpBase->mpMainConfig->Save();
    }
    mOriginalFocusWait = gpBase->mpEngine->GetWaitIfAppOutOfFocus();
    const char* smoke = std::getenv("AMNESIA_NET_SMOKE_SECONDS");
    if (smoke && Port(smoke, mSmokeSeconds) && mSmokeSeconds <= 900)
        mSmokeStart = std::chrono::steady_clock::now();
    else mSmokeSeconds = 0;
    mTestMovement = mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MOVEMENT");
    if(mSmokeSeconds) { const char* test = std::getenv("AMNESIA_NET_TEST_RECOVERY"); if(test) mRecoveryTest = std::string(test) == "migrate" ? 2 : std::string(test) == "reconnect" ? 1 : 0; }
    const char* drive = std::getenv("AMNESIA_NET_TEST_DRIVE_FILE");
    if(mSmokeSeconds && drive && *drive) {
        mQADriveFile = drive;
        NetLog("[NET/QA] Input driver enabled; original physics and campaign scripts retained");
    }
    cImGuiConsole::RegisterCommand("net_host", "net_host [port] - listen (connectivity prototype)",
        [this](const cImGuiConsole::tArgs& args) {
            uint16_t port = online::DefaultPort;
            if (args.size() > 1 || (!args.empty() && !Port(args[0], port))) { NetLog("[NET] Usage: net_host [1..65535]"); return; }
            Host(port);
        });
    cImGuiConsole::RegisterCommand("net_join", "net_join IP [port] - connect (connectivity prototype)",
        [this](const cImGuiConsole::tArgs& args) {
            uint16_t port = online::DefaultPort;
            if (args.empty() || args.size() > 2 || (args.size() == 2 && !Port(args[1], port))) { NetLog("[NET] Usage: net_join IP [1..65535]"); return; }
            Join(args[0], port);
        });
    cImGuiConsole::RegisterCommand("net_disconnect", "net_disconnect - close session",
        [this](const cImGuiConsole::tArgs&) { Disconnect(); NetLog("[NET/CONNECTION] Session closed"); });
    cImGuiConsole::RegisterCommand("net_status", "net_status - role and handshake status",
        [this](const cImGuiConsole::tArgs&) {
            const char* role = mSession.GetRole() == online::Role::Host ? "host" : mSession.GetRole() == online::Role::Client ? "client" : "offline";
            NetLog(std::string("[NET] role=") + role + " connected=" + (mSession.Connected() ? "yes" : "no") + " protocol=" + std::to_string(online::ProtocolVersion) + "; epoch=" + std::to_string(mEpoch));
        });
    cImGuiConsole::RegisterCommand("net_ping", "net_ping - ENet round trip milliseconds",
        [this](const cImGuiConsole::tArgs&) { NetLog("[NET] RTT=" + std::to_string(mSession.Ping()) + " ms"); });
    // Environment leaves Amnesia's legacy init-config command line intact.
    const char* role = std::getenv("AMNESIA_NET_ROLE");
    if (!role) {
        if (mSmokeSeconds) gpBase->mpEngine->SetWaitIfAppOutOfFocus(false);
        return;
    }
    uint16_t port = online::DefaultPort;
    const char* portText = std::getenv("AMNESIA_NET_PORT");
    if (portText && !Port(portText, port)) { NetLog("[NET] Invalid AMNESIA_NET_PORT"); return; }
    if (std::string(role) == "host") Host(port);
    else if (std::string(role) == "client") {
        const char* address = std::getenv("AMNESIA_NET_ADDRESS");
        Join(address ? address : "127.0.0.1", port);
    } else NetLog("[NET] AMNESIA_NET_ROLE must be host or client");
    if (mSession.GetRole() != online::Role::Offline || mSmokeSeconds)
        gpBase->mpEngine->SetWaitIfAppOutOfFocus(false);
}
void cLuxNetwork::Update(float timeStep) {
    luxsteam::Update();
    gpBase->mpEngine->SetWaitIfAppOutOfFocus(
        mSession.GetRole() == online::Role::Offline && !mSmokeSeconds ? mOriginalFocusWait : false);
    mSession.Poll();
    if(!mTestLanternSounds.empty()) {
        // HPL starts a newly created GUI entry in the following sound update.
        // Validate its pointer and ID before inspecting that live channel.
        auto* handler = gpBase->mpEngine->GetSound()->GetSoundHandler();
        for(auto it = mTestLanternSounds.begin(); it != mTestLanternSounds.end();) {
            const bool valid = handler->IsValid(it->entry, it->id);
            auto* channel = valid ? it->entry->GetChannel() : NULL;
            const bool playing = channel && channel->IsPlaying() && channel->GetVolume() > 0;
            if(playing) {
                NetLog("[NET/TEST] Owner lantern audio channel playing=1 file=" + it->asset + " target=2");
                it = mTestLanternSounds.erase(it);
            } else if(!valid || std::chrono::steady_clock::now()-it->queuedAt > std::chrono::seconds(1)) {
                NetLog("[NET/TEST] Owner lantern audio channel FAILED file=" + it->asset);
                it = mTestLanternSounds.erase(it);
            } else ++it;
        }
    }
    if(IsClientReplica() && mSession.PartnerLeftVoluntarily()) {
        NetLog("[NET/CONNECTION] Host left voluntarily; returning to main menu"); Disconnect();
    }
    const auto connectionNow = std::chrono::steady_clock::now();
    online::PlayerInput freshInput;
    const bool hostRole = mSession.GetRole() == online::Role::Host;
    if(hostRole && mSession.ReadInput(freshInput)) {
        // Field diagnostic: input that stopped arriving for a while, i.e. a
        // latency spike on the path or a stalled client. ReadInput already
        // treats input older than 250 ms as missing.
        if(mInputLeaseSeen && connectionNow - mInputLeaseAt > std::chrono::milliseconds(500))
            NetLog("[NET/DIAG] Client input resumed after a " +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(connectionNow - mInputLeaseAt).count() + 250) +
                " ms gap; rtt_ms=" + std::to_string(mSession.Ping()));
        mInputLeaseSeen = true; mInputLeaseAt = connectionNow;
    }
    // Pause the campaign only after 3 s without client input. With 1 s, every
    // latency spike past a second froze the whole world for both players; on
    // resume the client saw doors, enemies and its partner seconds late and was
    // pulled back to where the host last had it (2026-10-22 report). Between
    // 250 ms and 3 s the host keeps simulating and P2 simply stands still.
    const bool leasePaused = hostRole && mSession.IsWorldApplied(mEpoch) &&
        mInputLeaseSeen && connectionNow - mInputLeaseAt > std::chrono::seconds(3);
    if(leasePaused != mInputLeasePaused)
        NetLog(leasePaused ? "[NET/DIAG] No client input for 3 s; campaign paused until it returns" :
            "[NET/DIAG] Client input returned; campaign resumed");
    mInputLeasePaused = leasePaused;
    if(mSession.Connected()) {
        if(mConnectionGeneration != mSession.Generation()) {
            mConnectionGeneration = mSession.Generation();
            mMapAnnounced = false; mAppliedJournal = false; mJournalBytes.clear();
            mInputLeaseSeen = mInputLeasePaused = false; mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0;
            mJournalTestLastEpoch = 0;
            if(IsClientReplica()) { mEpoch = 0; mBaselineApplied = false; mAppliedInventory = false; }
            if(mRecovering) NetLog("[NET/RECOVERY] Partner reconnected; rebuilding authoritative baseline");
        }
        mEverConnected = true; mRecovering = false;
        mMigrationHost = false; mMigrationAttempted = false;
    } else if(mEverConnected && mSession.GetRole() != online::Role::Offline) {
        if(mSession.PartnerLeftVoluntarily()) mInputLeaseSeen = mInputLeasePaused = false;
        if(!mRecovering) { mRecovering = true; mRecoveryStart = connectionNow; mRetryAt = connectionNow; NetLog("[NET/RECOVERY] Connection lost; campaign paused for recovery"); }
        if(IsClientReplica() && mSession.RetryAllowed() && !mSession.AttemptActive() && connectionNow >= mRetryAt) {
            mSession.StartClient(mConnectAddress, mConnectPort, true); mRetryAt = connectionNow + std::chrono::seconds(1);
        }
        const auto missing = connectionNow - mRecoveryStart;
        if(!mMigrationHost && !mMigrationAttempted && mSession.RetryAllowed() && !mSession.PartnerLeftVoluntarily()) {
            if(IsClientReplica() && missing >= std::chrono::seconds(12) && !mRecoveryCheckpoint.bytes.empty()) {
                mMigrationAttempted = true;
                if(!PromoteFromCheckpoint()) NetLog("[NET/RECOVERY] Promotion failed; campaign remains paused");
            } else if(mSession.GetRole() == online::Role::Host && missing >= std::chrono::seconds(8) && mSession.CheckpointAcknowledged()) {
                const auto address = mSession.RemoteAddress(); const auto port = mSession.RemotePort();
                mConnectAddress = address; mConnectPort = port;
                mSession.StartClient(address,port,true); mWasClient = true; mHadClientConnection = true;
                mMigrationAttempted = true;
                NetLog("[NET/RECOVERY] Former host yielded authority; reconnecting to partner");
            }
        }
    }
    if(IsClientReplica()) {
        mWasClient = true;
        if(mSession.Connected()) mHadClientConnection = true;
    }
    if(mWasClient && !IsClientReplica()) {
        mWasClient = mHadClientConnection = false;
        gpBase->mpEngine->GetUpdater()->BroadcastMessageToAll(eUpdateableMessage_Reset);
        gpBase->mpInputHandler->ChangeState(eLuxInputState_MainMenu);
        gpBase->mpEngine->GetUpdater()->SetContainer("MainMenu");
        NetLog("[NET/CONNECTION] Client session ended; replica world unloaded");
    }
    const auto replicationStart = std::chrono::steady_clock::now();
    UpdateReplication(timeStep);
    if(mSession.GetRole() != online::Role::Offline) {
        const auto replicationEnd = std::chrono::steady_clock::now();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(replicationEnd-replicationStart).count();
        mReplicationTotalUs += us; mReplicationMaxUs = std::max(mReplicationMaxUs,int64_t(us)); ++mReplicationSamples;
        if(!mReplicationStatsAt.time_since_epoch().count()) mReplicationStatsAt = replicationEnd;
        if(replicationEnd-mReplicationStatsAt >= std::chrono::seconds(5)) {
            NetLog("[NET/PERF] Replication role=" + std::string(IsClientReplica() ? "client" : "host") +
                " average_us=" + std::to_string(mReplicationTotalUs/mReplicationSamples) + " max_us=" + std::to_string(mReplicationMaxUs));
            if(IsClientReplica() && mSnapshotGapSamples) {
                NetLog("[NET/STATS] Player state intervals average_ms=" + std::to_string(mSnapshotGapTotalMs/mSnapshotGapSamples) +
                    " max_ms=" + std::to_string(mSnapshotGapMaxMs) + " samples=" + std::to_string(mSnapshotGapSamples));
                mSnapshotGapTotalMs = mSnapshotGapMaxMs = 0; mSnapshotGapSamples = 0;
            }
            mReplicationStatsAt = replicationEnd; mReplicationTotalUs = mReplicationMaxUs = mReplicationSamples = 0;
        }
    }
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_VISIBILITY_PROPS")) UpdateVisibilityPropsTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_EFFECTS")) UpdateEffectsTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_STAIRS")) UpdateStairsTest(gpBase->mpMapHandler->GetCurrentMap(),timeStep);
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_GATE")) UpdateGateTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_LEVERS")) UpdateLeverTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_FLASHBACK")) UpdateFlashbackTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_FAIRNESS")) UpdateFairnessTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_DOOR")) UpdateDoorTest(gpBase->mpMapHandler->GetCurrentMap());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_ITEM_PATH")) UpdateItemPathTest();
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_SPAWN") && !IsClientReplica() && !mSpawnTestStarted &&
        mSession.Connected() && mSession.IsWorldApplied(mEpoch) && gpBase->mpPlayer2 && gpBase->mpMapHandler->GetCurrentMap()) {
        // A crouch-height crawl space with no room around the host (as under the
        // 15_prison_south hole): recreating P2 must not close the session.
        const auto now = std::chrono::steady_clock::now();
        if(!mSpawnTestAt.time_since_epoch().count()) mSpawnTestAt = now;
        else if(now - mSpawnTestAt >= std::chrono::seconds(2)) {
            mSpawnTestStarted = true;
            cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
            auto* world = map->GetPhysicsWorld();
            const cVector3f origin(100, 30, 100);
            world->SetWorldSize(cVector3f(-200,-100,-200), cVector3f(200,100,200));
            auto box = [&](const char* name, cVector3f size, cVector3f position) {
                auto* body = world->CreateBody(name, world->CreateBoxShape(size, NULL));
                body->SetMass(0); body->SetPosition(origin + position); body->SetCollideCharacter(true);
            };
            box("online_qa_crawl_floor", cVector3f(4, .3f, 4), cVector3f(0, -.15f, 0));
            box("online_qa_crawl_ceiling", cVector3f(4, .3f, 4), cVector3f(0, 1.45f, 0));
            box("online_qa_crawl_wall_n", cVector3f(1.5f, 1.3f, .2f), cVector3f(0, .65f, -.65f));
            box("online_qa_crawl_wall_s", cVector3f(1.5f, 1.3f, .2f), cVector3f(0, .65f, .65f));
            box("online_qa_crawl_wall_e", cVector3f(.2f, 1.3f, 1.5f), cVector3f(.65f, .65f, 0));
            box("online_qa_crawl_wall_w", cVector3f(.2f, 1.3f, 1.5f), cVector3f(-.65f, .65f, 0));
            auto* host = gpBase->mpPlayer;
            static_cast<cLuxMoveState_Normal*>(host->GetMoveStateData(eLuxMoveState_Normal))->SetCrouch(true);
            host->GetCharacterBody()->SetFeetPosition(origin + cVector3f(0, .01f, 0));
            NetLog("[NET/TEST] Host crouched in a closed crawl space; recreating Player 2");
            gpBase->mpMapHandler->DestroyPlayer2(map);
            mRemotePlayer2 = NULL;
        }
    }
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_LEVEL_DOOR") && !IsClientReplica() && !mLevelDoorTestStarted &&
        mSession.Connected() && mSession.IsWorldApplied(mEpoch) && gpBase->mpMapHandler->GetCurrentMap()) {
        // 12_storage's door back to 09_back_hall, verbatim from the original map.
        const auto now = std::chrono::steady_clock::now();
        if(!mLevelDoorTestAt.time_since_epoch().count()) mLevelDoorTestAt = now;
        else if(now - mLevelDoorTestAt >= std::chrono::seconds(2)) {
            mLevelDoorTestStarted = true;
            // First a 4 s game-thread stall with no load notice (as when the notice
            // was lost on 2026-10-22): the client must wait, not drop the session.
            NetLog("[NET/TEST] Host game thread stalled 4 s without load notice");
            std::this_thread::sleep_for(std::chrono::seconds(4));
            NetLog("[NET/TEST] Host using level door map file C:/SVN/amnesia/redist/maps/main/ch01/09_back_hall.map");
            gpBase->mpMapHandler->ChangeMap("C:/SVN/amnesia/redist/maps/main/ch01/09_back_hall.map", "", "", "");
        }
    }
    UpdateRecoveryTest();
    UpdateMenuTest();
    UpdateCampaignStartTest();
    UpdateCheckpoint(timeStep);
    if(IsClientReplica() && gpBase->mpEngine->GetUpdater()->GetCurrentContainerName() != "Default") {
        online::PlayerInput idle; auto* camera = gpBase->mpPlayer->GetCamera();
        idle.yaw = int16_t(std::atan2(std::sin(camera->GetYaw()),std::cos(camera->GetYaw()))*1000);
        idle.pitch = int16_t(std::max(-1.571f,std::min(1.571f,camera->GetPitch()))*1000);
        SendLocalInput(idle,timeStep);
    }
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(map && mSession.Connected() && !gpBase->mpMapHandler->GetCoopMode()) {
        map->GetWorld()->SetActive(true);
        if(gpBase->mpEngine->GetUpdater()->GetCurrentContainerName() != "Default") {
            gpBase->mpEngine->GetUpdater()->RunMessageOnContainer("Default", eUpdateableMessage_Update, timeStep);
            gpBase->mpEngine->GetUpdater()->RunMessageOnContainer("Default", eUpdateableMessage_PostUpdate, timeStep);
            map = gpBase->mpMapHandler->GetCurrentMap();
        }
    }
    if (mRemotePlayer2 && mRemotePlayer2 != gpBase->mpPlayer2) mRemotePlayer2 = NULL;
    // Save loading can rebuild an online P2 without enabling split-screen.
    if(!mRemotePlayer2 && mSession.Connected() && gpBase->mpPlayer2 && !gpBase->mpMapHandler->GetCoopMode()) mRemotePlayer2 = gpBase->mpPlayer2;
    if (mSession.Connected() && map &&
        gpBase->mpPlayer->GetCharacterBody()) {
        if (!gpBase->mpPlayer2 && !gpBase->mpMapHandler->GetCoopMode()) {
            gpBase->mpMapHandler->CreatePlayer2(map);
            mRemotePlayer2 = gpBase->mpPlayer2;
            if(!IsClientReplica() && mFillRemoteStartingOil) {
                mRemotePlayer2->SetLampOil(100); mFillRemoteStartingOil = false;
                NetLog("[NET/LANTERN] New campaign Player 2 starts with oil=100");
            }
            if (!IsClientReplica() && !PlaceRemotePlayer2()) {
                gpBase->mpMapHandler->DestroyPlayer2(map);
                mRemotePlayer2 = NULL;
                NetLog("[NET/PLAYER] No safe remote spawn near Player 1; closing session");
                mSession.Stop();
                return;
            }
            mPreviousButtons = 0;
            mObservedRemoteMovement = false;
            mLoggedRemoteInput = false;
            mRemoteSpawnFeet = mRemotePlayer2->GetCharacterBody()->GetFeetPosition();
            NetLog(IsClientReplica() ? "[NET/PLAYER] Created host avatar in replica world" : "[NET/PLAYER] Created authoritative Player 2; full-screen host viewport retained");
        } else if (OwnsRemotePlayer2() && !mRemotePlayer2->GetCharacterBody()) {
            // MapHandler destroys P2 world entities on map leave. Reattach only
            // after P1 has entered and acquired a body in the new world.
            mRemotePlayer2->CreateWorldEntities(map);
            mRemotePlayer2->OnMapEnter(map);
            mRemotePlayer2->ReenterCurrentMoveState();
            gpBase->mpMapHandler->PlacePlayer2NearPlayer1();
            if (!IsClientReplica() && !PlaceRemotePlayer2()) {
                gpBase->mpMapHandler->DestroyPlayer2(map);
                mRemotePlayer2 = NULL;
                mSession.Stop();
                return;
            }
            mPreviousButtons = 0;
        }
    } else if (OwnsRemotePlayer2() && mSession.GetRole() == online::Role::Offline) {
        if (mSmokeSeconds && mRemotePlayer2->GetCharacterBody()) {
            cVector3f delta = mRemotePlayer2->GetCharacterBody()->GetFeetPosition() - mRemoteSpawnFeet;
            NetLog("[NET/TEST] P2 displacement=" + std::to_string(delta.x) + "," + std::to_string(delta.y) + "," + std::to_string(delta.z) +
                " bodyActive=" + (mRemotePlayer2->GetCharacterBody()->IsActive() ? "yes" : "no") +
                " speed=" + std::to_string(mRemotePlayer2->GetCharacterBody()->GetMoveSpeed(eCharDir_Forward)));
        }
        gpBase->mpMapHandler->DestroyPlayer2(map);
        mRemotePlayer2 = NULL;
        mPreviousButtons = 0;
        NetLog("[NET/PLAYER] Removed remote Player 2");
    }
    if (mSmokeSeconds && std::chrono::steady_clock::now() - mSmokeStart >= std::chrono::seconds(mSmokeSeconds)) {
        mSmokeSeconds = 0;
        NetLog("[NET/TEST] Smoke test finished; exiting cleanly");
        mSession.CloseVoluntarily(); // QA timer completion is an intentional exit, not a simulated crash.
        gpBase->mpEngine->Exit();
    }
}
bool cLuxNetwork::OwnsRemotePlayer2() const {
    return mRemotePlayer2 && mRemotePlayer2 == gpBase->mpPlayer2;
}
void cLuxNetwork::UpdateMenuTest() {
    if(!mSmokeSeconds || !TestFlag("AMNESIA_NET_TEST_MENU") || mMenuTestStage >= 9) return;
    const char* role = std::getenv("AMNESIA_NET_ROLE");
    const bool client = role && std::string(role) == "client";
    const auto now = std::chrono::steady_clock::now();
    const float age = std::chrono::duration<float>(now-mMenuTestAt).count();
    auto fail = [&](const char* reason) {
        NetLog(std::string("[NET/TEST] Online menu FAILED: ")+reason);
        mMenuTestStage = 99; gpBase->mpEngine->Exit();
    };
    auto* mapHandler = gpBase->mpMapHandler;
    auto* menu = gpBase->mpMainMenu;
    if(mMenuTestStage == 0) {
        if(!mSession.Connected() || !mapHandler->MapIsLoaded() || !(client ? mBaselineApplied : mBaselineSent) ||
            std::chrono::duration<float>(now-mSmokeStart).count() < 5 ||
            (client ? mRecoveryCheckpoint.bytes.empty() : !mSession.CheckpointAcknowledged())) return;
        gpBase->mpEffectHandler->GetImageTrail()->FadeTo(1.8f,100);
        gpBase->mpEffectHandler->GetRadialBlur()->FadeTo(0.12f,100);
        mMenuTestStage = 1; mMenuTestAt = now; return;
    }
    if(mMenuTestStage == 1 && age > 0.3f) {
        cPostEffectParams_ImageTrail trail; mapHandler->GetPostEffect_ImageTrail()->GetParams(&trail);
        cPostEffectParams_RadialBlur radial; mapHandler->GetPostEffect_RadialBlur()->GetParams(&radial);
        if(trail.mfAmount < 1 || radial.mfSize <= 0 || mapHandler->GetPostEffect_ImageTrail()->IsActive() || mapHandler->GetPostEffect_RadialBlur()->IsActive()) {
            fail("clear image failed to suppress rendering while preserving script effects"); return;
        }
        gpBase->mpEngine->GetUpdater()->SetContainer("MainMenu");
        menu->SetWindowActive(eLuxMainMenuWindow_Online);
        auto* check = static_cast<cWidgetCheckBox*>(menu->GetSet()->GetWidgetFromName("OnlineClearImage"));
        if(!check) { fail("missing clear image checkbox"); return; }
        check->SetChecked(false,true);
        mMenuTestStage = 2; mMenuTestAt = now; return;
    }
    if(mMenuTestStage == 2 && age > 0.3f) {
        if(!mapHandler->GetPostEffect_ImageTrail()->IsActive() || !mapHandler->GetPostEffect_RadialBlur()->IsActive() || ClearImagePreference()) {
            fail("original effects were not restored by checkbox"); return;
        }
        auto* check = static_cast<cWidgetCheckBox*>(menu->GetSet()->GetWidgetFromName("OnlineClearImage"));
        check->SetChecked(true,true);
        mMenuTestStage = 3; mMenuTestAt = now; return;
    }
    if(mMenuTestStage == 3 && age > 0.3f) {
        if(mapHandler->GetPostEffect_ImageTrail()->IsActive() || mapHandler->GetPostEffect_RadialBlur()->IsActive() || !ClearImagePreference()) {
            fail("clear image did not reactivate"); return;
        }
        NetLog("[NET/TEST] Clear image checkbox suppresses and restores original blur without changing graphics preferences");
        if(!client) { menu->ExitMenu(eLuxMainMenuExit_ReturnToGame); mMenuTestStage = 8; return; }
        menu->ExitMenu(eLuxMainMenuExit_QuitToMenu);
        mMenuTestStage = 4; return;
    }
    if(mMenuTestStage == 4 && mSession.GetRole() == online::Role::Offline && !mapHandler->MapIsLoaded()) {
        mMenuTestAt = now; mMenuTestStage = 5; return;
    }
    if(mMenuTestStage == 5) {
        if(mSession.GetRole() != online::Role::Offline || mapHandler->MapIsLoaded() || gpBase->mpEngine->GetUpdater()->GetCurrentContainerName() != "MainMenu") {
            fail("exit returned to match automatically"); return;
        }
        if(age < (mMenuTestExits ? 2 : 10)) return;
        NetLog(mMenuTestExits ? "[NET/TEST] Save and exit stays at main menu with transport stopped" : "[NET/TEST] Exit stays at main menu with transport stopped");
        menu->SetWindowActive(eLuxMainMenuWindow_Online);
        mMenuTestStage = 6; mMenuTestAt = now; return;
    }
    if(mMenuTestStage == 6 && age > 0.2f) {
        auto* button = menu->GetSet()->GetWidgetFromName("OnlineReconnect");
        if(!button || !button->ProcessMessage(eGuiMessage_ButtonPressed,cGuiMessageData(0))) {
            fail("reconnect button unavailable"); return;
        }
        mMenuTestStage = 7; mMenuTestAt = now; return;
    }
    if(mMenuTestStage == 7 && mSession.Connected() && mBaselineApplied && mapHandler->MapIsLoaded()) {
        NetLog("[NET/TEST] Reconnect button restored complete campaign baseline");
        if(++mMenuTestExits < 2) {
            gpBase->mpEngine->GetUpdater()->SetContainer("MainMenu");
            menu->ExitMenu(eLuxMainMenuExit_QuitAndSave); mMenuTestStage = 4;
        } else { mMenuTestStage = 9; NetLog("[NET/TEST] Online menu exit/reconnect regression completed"); }
    }
    if(!client && mMenuTestStage == 8 && mSession.Generation() >= 3 && mBaselineSent) {
        mMenuTestStage = 9; NetLog("[NET/TEST] Online menu exit/reconnect regression completed");
    }
}
void cLuxNetwork::UpdateCampaignStartTest() {
    if(!mSmokeSeconds || !TestFlag("AMNESIA_NET_TEST_CAMPAIGN_START")) return;
    auto* map = gpBase->mpMapHandler->GetCurrentMap();
    const float age = std::chrono::duration<float>(std::chrono::steady_clock::now()-mSmokeStart).count();
    if(mCampaignStartTestStage == 0 && !IsClientReplica() && mSession.LobbyReady() && !map && age > 12) {
        auto* menu = gpBase->mpMainMenu;
        menu->SetWindowActive(eLuxMainMenuWindow_Online);
        auto* button = menu->GetSet()->GetWidgetFromName("OnlineNewCampaign");
        if(!button || !button->ProcessMessage(eGuiMessage_ButtonPressed,cGuiMessageData(0))) {
            NetLog("[NET/TEST] Campaign start FAILED: new campaign button unavailable"); gpBase->mpEngine->Exit(); return;
        }
        mCampaignStartTestStage = 1;
        NetLog("[NET/TEST] Campaign started through online lobby new campaign button");
    }
    if(!map || !mSession.IsWorldApplied(mEpoch)) return;
    if(!mTestStartingOilObserved) {
        const bool full = gpBase->mpPlayer->GetLampOil() > 99.99f &&
            (IsClientReplica() || (gpBase->mpPlayer2 && gpBase->mpPlayer2->GetLampOil() > 99.99f));
        NetLog(full ? "[NET/TEST] Original first level starts with full oil on both players" : "[NET/TEST] First-level starting oil FAILED");
        mTestStartingOilObserved = true;
    }
    if(!IsClientReplica() && mCampaignStartTestStage == 1) {
        const auto* step = map->GetVar("BlackoutStep");
        if(step && cString::ToInt(step->msVal.c_str(),0) >= 18 && gpBase->mpPlayer->IsActive()) {
            mCampaignStartTestStage = 2;
            NetLog("[NET/TEST] Original campaign intro completed; native timers released controls");
        }
    }
}
bool cLuxNetwork::UpdateQADrive(float timeStep) {
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_LEVERS") && IsClientReplica()) return UpdateLeverDrive(timeStep);
    if(mSmokeSeconds && (TestFlag("AMNESIA_NET_TEST_EFFECTS") || TestFlag("AMNESIA_NET_TEST_STAIRS"))) return true;
    if(mQADriveFile.empty() || !mSmokeSeconds) return false;
    cLuxPlayer* player = gpBase->mpPlayer;
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(!map || !player || !player->GetCharacterBody()) return true;
    FILE* command = std::fopen(mQADriveFile.c_str(), "rb");
    if(command) {
        unsigned serial; int frames, forward, strafe, yaw, pitch, interact, attack = 0;
        const int fields = std::fscanf(command, "%u %d %d %d %d %d %d %d", &serial, &frames, &forward, &strafe, &yaw, &pitch, &interact, &attack);
        if((fields == 7 || fields == 8) &&
            serial > mQADriveSerial && frames >= 0 && frames <= 120 && forward >= -1000 && forward <= 1000 && strafe >= -1000 && strafe <= 1000 &&
            yaw >= -3142 && yaw <= 3142 && pitch >= -1571 && pitch <= 1571 && (interact == 0 || interact == 1) && (attack == 0 || attack == 1)) {
            mQADriveSerial = serial; mQADriveFrames = frames;
            mQADriveInput.forward = int16_t(forward); mQADriveInput.strafe = int16_t(strafe);
            mQADriveInput.yaw = int16_t(yaw); mQADriveInput.pitch = int16_t(pitch);
            mQADriveInteract = interact != 0;
            mQADriveAttack = attack != 0;
        }
        std::fclose(command);
    }
    const bool active = mQADriveFrames > 0 && player->IsActive() && !player->IsDead() && !WaitingForMap();
    if(mQADriveFrames > 0) --mQADriveFrames;
    online::PlayerInput input = mQADriveInput;
    if(!active) { input.forward = input.strafe = 0; }
    if(active) {
        float delta = input.yaw / 1000.0f - player->GetCamera()->GetYaw();
        // Slow the QA feedback loop while it observes a delayed scripted camera.
        const float gain = IsClientReplica() && mClientScriptLook ? 0.05f : 0.5f;
        const float yawAdd = std::atan2(std::sin(delta), std::cos(delta)) * gain;
        const float pitchAdd = (input.pitch / 1000.0f - player->GetCamera()->GetPitch()) * 0.5f;
        input.lookYaw = int16_t(yawAdd * 10000); input.lookPitch = int16_t(pitchAdd * 10000);
        if(!ClientObjectControlsLook()) { player->AddYaw(yawAdd); player->AddPitch(pitchAdd); }
    }
    // Send the actual constrained view, just as physical mouse input does.
    const float driveYaw = player->GetCamera()->GetYaw();
    input.yaw = int16_t(std::atan2(std::sin(driveYaw), std::cos(driveYaw)) * 1000);
    input.pitch = int16_t(std::max(-1.571f, std::min(1.571f, player->GetCamera()->GetPitch())) * 1000);
    if(IsClientReplica()) SendLocalInput(input, timeStep);
    else if(active) {
        if(input.forward) player->Move(eCharDir_Forward, input.forward / 1000.0f);
        if(input.strafe) player->Move(eCharDir_Right, input.strafe / 1000.0f);
    }
    bool held = active && mQADriveInteract;
    if(held != mQADrivePreviousInteract) {
        if(IsClientReplica()) SendLocalAction(online::PlayerAction::Interact, held);
        else player->DoAction(eLuxPlayerAction_Interact, held);
        mQADrivePreviousInteract = held;
        NetLog(std::string("[NET/QA] Interact=") + (held ? "pressed" : "released") + " focus=" +
            (player->GetEntityInFocus() ? player->GetEntityInFocus()->GetName() : "none"));
    }
    const bool attack = active && mQADriveAttack;
    if(attack != mQADrivePreviousAttack) {
        if(IsClientReplica()) SendLocalAction(online::PlayerAction::Attack, attack);
        else player->DoAction(eLuxPlayerAction_Attack, attack);
        mQADrivePreviousAttack = attack;
    }
    mQADriveTelemetryTime += timeStep;
    if(mQADriveTelemetryTime >= 0.25f) {
        mQADriveTelemetryTime = 0;
        FILE* state = std::fopen((mQADriveFile + ".state").c_str(), "wb");
        if(state) {
            cVector3f feet = player->GetCharacterBody()->GetFeetPosition();
            std::fprintf(state, "%u %s %.4f %.4f %.4f %.4f %.4f %d %.2f %s\n", mEpoch, map->GetFileName().c_str(), feet.x, feet.y, feet.z,
                player->GetCamera()->GetYaw(), player->GetCamera()->GetPitch(), player->IsActive() ? 1 : 0, player->GetHealth(),
                player->GetEntityInFocus() ? player->GetEntityInFocus()->GetName().c_str() : "none");
            std::fclose(state);
        }
        FILE* acknowledgement = std::fopen((mQADriveFile + ".ack").c_str(), "wb");
        if(acknowledgement) { std::fprintf(acknowledgement, "%u\n", mQADriveSerial); std::fclose(acknowledgement); }
        if(!IsClientReplica() && gpBase->mpPlayer2 && gpBase->mpPlayer2->GetCharacterBody()) {
            FILE* peer = std::fopen((mQADriveFile + ".peer").c_str(), "wb");
            if(peer) {
                cLuxPlayer* p = gpBase->mpPlayer2; cVector3f feet = p->GetCharacterBody()->GetFeetPosition();
                std::fprintf(peer, "%.4f %.4f %.4f %.4f %.4f %.4f %d\n", feet.x, feet.y, feet.z,
                    p->GetCamera()->GetYaw(), p->GetCamera()->GetPitch(), p->GetLookSpeedMul(), int(p->GetCurrentState()));
                std::fclose(peer);
            }
            FILE* motion = std::fopen((mQADriveFile + ".peer.motion").c_str(), "wb");
            if(motion) {
                auto* body = gpBase->mpPlayer2->GetCharacterBody();
                std::fprintf(motion, "%.5f %.5f\n", body->GetMoveSpeed(eCharDir_Forward), body->GetMoveSpeed(eCharDir_Right));
                std::fclose(motion);
            }
        }
    }
    return true;
}
bool cLuxNetwork::PlaceRemotePlayer2() {
    iCharacterBody* body = mRemotePlayer2->GetCharacterBody();
    iCharacterBody* hostBody = gpBase->mpPlayer->GetCharacterBody();
    iPhysicsWorld* world = gpBase->mpMapHandler->GetCurrentMap()->GetPhysicsWorld();
    // Local co-op's unconditional one-metre offset can put P2 inside a wall.
    // Test the actual character shape against the host's world before spawning.
    auto fits = [&](const cVector3f& feet) {
        const cVector3f centre = feet + cVector3f(0, body->GetCurrentShape()->GetSize().y / 2, 0);
        return !world->CheckShapeWorldCollision(NULL, body->GetCurrentShape(), cMath::MatrixTranslate(centre), NULL, false, true, NULL, false);
    };
    auto place = [&](const cVector3f& feet) { body->SetFeetPosition(feet); body->Update(0.001f); };
    // Small lifts clear uneven floors and steps; gravity settles the body.
    const float lifts[] = { 0.03f, 0.2f, 0.4f };
    const float radii[] = { 1.0f, 0.8f, 1.3f, 0.6f, 1.6f, 2.0f };
    for (float lift : lifts) {
        for (float radius : radii) {
            for (int direction = 0; direction < 8; ++direction) {
                float angle = hostBody->GetYaw() + direction * 0.785398163f;
                cVector3f feet = hostBody->GetFeetPosition() + cVector3f(std::cos(angle) * radius, lift, -std::sin(angle) * radius);
                if (fits(feet)) { place(feet); return true; }
            }
        }
    }
    // Nothing free around the host -- a crawl tunnel or a shaft, such as the
    // landing under the 15_prison_south hole (2026-10-22: every load of that
    // save closed the session). Online players never collide with each other,
    // and the host's own shape already fits where it stands, so share that
    // spot, crouching P2 when the host is crouched under a low ceiling. Never
    // refuse: a slightly imperfect spawn is pushed out by the character body,
    // while closing the session made the save unplayable.
    const cVector3f hostFeet = hostBody->GetFeetPosition() + cVector3f(0, 0.01f, 0);
    if (hostBody->GetActiveSize() == 1 && mRemotePlayer2->GetCurrentMoveState() == eLuxMoveState_Normal) {
        static_cast<cLuxMoveState_Normal*>(mRemotePlayer2->GetMoveStateData(eLuxMoveState_Normal))->SetCrouch(true);
    }
    NetLog(std::string("[NET/PLAYER] Remote spawn fallback: sharing the host position") +
        (body->GetActiveSize() == 1 ? " crouched" : "") + (fits(hostFeet) ? "" : " (shape still touching; body will resolve)"));
    place(hostFeet);
    return true;
}
// Esegue i comandi non ancora confermati con il corpo del personaggio del
// client in modalità di previsione (iCharacterBody::SetPredicting): stessa
// accelerazione, attrito, gradini, pendenze e collisioni dell'host, senza
// toccare telecamera, entità, oggetti spinti o callback. Lo stato del corpo
// viene salvato prima e ripristinato dopo.
struct NativeStepper {
    iCharacterBody* body;
    iCharacterBody::tPredictionState saved;
    explicit NativeStepper(iCharacterBody* b) : body(b) {}
    void Begin(const online::PlayerState& s) {
        saved = body->GetPredictionState();
        auto start = saved;
        start.mvPosition = start.mvLastPosition = cVector3f(s.x/1000.0f, s.y/1000.0f, s.z/1000.0f) + cVector3f(0, body->GetSize().y/2, 0);
        start.mvVelocity = start.mvForce = cVector3f(0);
        // Velocità e limiti di movimento dell'host per questo giocatore.
        start.mfMoveSpeed[eCharDir_Forward] = s.forwardSpeed/1000.0f; start.mfMoveSpeed[eCharDir_Right] = s.sideSpeed/1000.0f;
        start.mfMaxPosMoveSpeed[eCharDir_Forward] = s.forwardMax/1000.0f; start.mfMaxNegMoveSpeed[eCharDir_Forward] = -s.backwardMax/1000.0f;
        start.mfMaxPosMoveSpeed[eCharDir_Right] = s.sideMax/1000.0f; start.mfMaxNegMoveSpeed[eCharDir_Right] = -s.sideMax/1000.0f;
        for(int i = 0; i < 2; ++i) { start.mfMoveAcc[i] = s.acceleration/1000.0f; start.mfMoveDeacc[i] = s.deceleration/1000.0f; start.mbMoving[i] = false; }
        start.mfCheckStepClimbCount = 0; start.mbClimbing = false; start.mlOnGroundCount = s.grounded ? std::max(1, saved.mlOnGroundCount) : 0;
        start.mfMoveDelayCount = 0;
        start.mpGravityAttachedBody = start.mpGravityLastAttachedBody = NULL; start.mlGravityAttachmentContacts = 0;
        body->SetPredictionState(start); body->SetPredicting(true);
    }
    online::PredictedPosition Step(const online::PlayerInput& input, float dt) {
        body->SetYaw(input.yaw/1000.0f);
        // Come sull'host: un asse fermo non riceve Move (Move(0) toglie l'attrito).
        if(input.forward) body->Move(eCharDir_Forward, input.forward/1000.0f);
        if(input.strafe) body->Move(eCharDir_Right, input.strafe/1000.0f);
        body->Update(dt);
        const cVector3f position = body->GetPosition();
        return {position.x, position.z};
    }
    void End() { body->SetPredicting(false); body->SetPredictionState(saved); }
};
void cLuxNetwork::Reset() {
    mRemotePlayer2 = NULL; mPreviousButtons = 0;
    mInputAccumulator = 0;
    mPendingLookYaw = mPendingLookPitch = 0; mHasLookSequence = false;
    mHasLastLocalFeet = false;
    mClientObjectLook = mClientScriptLook = mRemoteRotate = mClientSharedIntro = false;
    mSnapshots.clear(); mReceivedSnapshot = false; mReceivedSequence = 0; mLastSnapshotAt = {};
    mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0; mDeferredActions.clear();
    mEntityBytes.clear(); mEntityStates.clear(); mReplicaIDs.clear(); mLoggedWorldReplica = false;
    mEntityMotionAt.clear(); mEntityMotionSettled.clear(); mEntitySentAt.clear(); mPropMotion.clear(); mEnemyMotion.clear();
    mFillRemoteStartingOil = false;
    mBaselineApplied = false; mBaselineSent = false; mClientMoved = false;
    mAppliedInventory = false; mInventoryBytes.clear();
    mAppliedJournal = false; mJournalBytes.clear();
    mPendingPresentation.clear(); mLightBytes.clear(); mLoggedLighting = false;
    for(auto& bytes : mFogBytes) bytes.clear();
    mMediaBytes.clear(); mMediaStates.clear(); mReplicaMediaIDs.clear(); mMusicFadeTimes.clear(); mMediaBaselineApplied = false;
    for(auto& bytes : mGlobalAudioBytes) bytes.clear();
    mClientDead = false; mHasClientLife = false; mHasHostLife = false;
    mSnapshotAccumulator = mSnapshotBackupAccumulator = mWorldReplicationAccumulator = 0;
    mSnapshotGapTotalMs = mSnapshotGapMaxMs = 0; mSnapshotGapSamples = 0;
}
void cLuxNetwork::SendLocalInput(online::PlayerInput input, float timeStep) {
    if(!mSession.Connected()) { mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0; return; }
    if (IsClientReplica() && (!mEpoch || !mReceivedSnapshot || !mBaselineApplied)) return;
    // AMNESIA_NET_TEST_INPUT_GAP: a 2 s gap must not pause the host; a 4 s gap must.
    if(IsClientReplica() && mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_INPUT_GAP")) {
        const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - mSmokeStart).count();
        if((elapsed >= 14 && elapsed < 16) || (elapsed >= 22 && elapsed < 26)) {
            if(!mTestInputGapLogged) { mTestInputGapLogged = true; NetLog("[NET/TEST] Client withholding input"); }
            return;
        }
        mTestInputGapLogged = false;
    }
    // AMNESIA_NET_TEST_LEAN: hold lean right; the host leans P2 and the roll
    // must come back to this camera through the snapshot.
    if(IsClientReplica() && mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_LEAN") && !mTestLeanLogged) {
        const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - mSmokeStart).count();
        if(elapsed >= 12 && elapsed < 15) {
            input.buttons |= 32;
            mTestLeanMaxRoll = std::max(mTestLeanMaxRoll, std::fabs(gpBase->mpPlayer->GetCamera()->GetRoll()));
        } else if(elapsed >= 15) {
            mTestLeanLogged = true;
            NetLog(std::string("[NET/TEST] ") + (mTestLeanMaxRoll > 0.05f ? "PASS" : "FAILED") +
                " client lean reached the camera max_roll=" + std::to_string(mTestLeanMaxRoll));
        }
    }
    if(IsClientReplica() && (mClientDead || mClientSharedIntro)) { input.forward = input.strafe = 0; input.buttons = 0; input.lookYaw = input.lookPitch = 0; }
    if(IsClientReplica() && !mTestMovement) { input.sequence = mInputSequence + 1; mPrediction.Push(input,timeStep); }
    const bool jumpEdge=((input.buttons^mLocalInputButtons)&1)!=0;
    if(jumpEdge) {
        if((input.buttons&1) && IsClientReplica() && !mSnapshots.empty())
            mVerticalPrediction.StartJump(mSnapshots.back().state.players[1],mInputSequence+1);
        SendLocalAction(online::PlayerAction::Jump,(input.buttons&1)!=0);
    }
    mLocalInputButtons=input.buttons;
    ++mInputTick;
    if(LegacyInput()) {
        // Confronti A/B: invio della 0.5.1, un pacchetto ogni due tick con
        // l'ultimo input e i delta del mouse accumulati.
        mInputAccumulator += timeStep;
        mPendingLookYaw = std::max(-31420, std::min(31420, mPendingLookYaw + int(input.lookYaw)));
        mPendingLookPitch = std::max(-31420, std::min(31420, mPendingLookPitch + int(input.lookPitch)));
        if (!jumpEdge && mInputAccumulator < 1.0f / 30.0f) return;
        mInputAccumulator = std::fmod(mInputAccumulator, 1.0f / 30.0f);
        input.lookYaw = int16_t(mPendingLookYaw); input.lookPitch = int16_t(mPendingLookPitch);
        mPendingLookYaw = mPendingLookPitch = 0;
    }
    // Protocollo 17: un comando per tick logico, con i propri delta del mouse,
    // inviato subito; ogni pacchetto ripete gli ultimi comandi (InputBatch) e
    // l'host ne simula uno per tick. Prima un pacchetto ogni due tick portava
    // solo l'ultimo input, che l'host applicava finché non ne arrivava un
    // altro: con il jitter durava 1 o 3 tick invece dei 2 previsti qui.
    input.sequence = ++mInputSequence; input.tick = mInputTick;
    input.epoch = mEpoch;
    if (mTestMovement) {
        if(mAppliedInventory && !mTestPotionRequested && gpBase->mpInventory->GetItem("online_test_potion")) {
            SendInventoryAction(online::PlayerAction::UseInventory, "online_test_potion");
            mTestPotionRequested = true;
        }
        input.forward = 1000;
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - mSmokeStart).count();
        input.yaw = int16_t(std::sin(seconds * 1.5f) * 3000); input.pitch = 0;
        gpBase->mpPlayer->GetCamera()->SetYaw(input.yaw / 1000.0f);
        iLuxEntity* item = gpBase->mpMapHandler->GetCurrentMap()->GetEntityByName("online_test_tinderbox");
        if(item && item->GetEntityType() == eLuxEntityType_Prop) {
            iLuxProp* prop = static_cast<iLuxProp*>(item);
            if(prop->GetBodyNum()) {
                cVector3f aim = prop->GetBody(0)->GetLocalPosition() - gpBase->mpPlayer->GetCamera()->GetPosition();
                input.forward = 0;
                input.yaw = int16_t(std::atan2(-aim.x, -aim.z) * 1000);
                input.pitch = int16_t(std::atan2(aim.y, std::sqrt(aim.x*aim.x + aim.z*aim.z)) * 1000);
                gpBase->mpPlayer->GetCamera()->SetYaw(input.yaw / 1000.0f);
                gpBase->mpPlayer->GetCamera()->SetPitch(input.pitch / 1000.0f);
                mTestActionTimer += timeStep;
                if(mTestActionTimer > 0.5f) { SendLocalAction(online::PlayerAction::Interact, true); SendLocalAction(online::PlayerAction::Interact, false); mTestActionTimer = 0; }
            }
        }
    }
    if(IsClientReplica() && mTestMovement && TestFlag("AMNESIA_NET_TEST_PREDICTION")) mPrediction.Push(input,timeStep);
    mSession.SendInput(input);
}
void cLuxNetwork::OnExit() {
    if(IsClientReplica() && mSnapshotGapSamples)
        NetLog("[NET/STATS] Player state intervals average_ms=" + std::to_string(mSnapshotGapTotalMs/mSnapshotGapSamples) +
            " max_ms=" + std::to_string(mSnapshotGapMaxMs) + " samples=" + std::to_string(mSnapshotGapSamples) + " final_window=1");
    mSession.Stop();
    gpBase->mpEngine->SetWaitIfAppOutOfFocus(mOriginalFocusWait);
}
bool cLuxNetwork::ApplyRemotePlayerInput() {
    if (mSession.GetRole() != online::Role::Host) return false;
    // Host networking owns the P2 input slot, including before/after connection.
    cLuxPlayer* player = gpBase->mpPlayer2;
    if (!player || !player->GetCharacterBody()) { mPreviousButtons = 0; return true; }
    // Un comando in coda per tick (protocollo 17); `repeated` ripete il
    // precedente mentre il buffer contro il jitter si riempie.
    online::PlayerInput input; bool repeated = false;
    const bool hasInput = !WaitingForMap() && !SharedIntroView() && (LegacyInput() ? mSession.ReadInput(input) : mSession.NextInput(input, repeated));
    const bool active = hasInput && player->IsActive() && !player->IsDead();
    ReportInputQueue(hasInput, repeated);
    // Tick senza il comando del client (rete in ritardo): il mouse non si è
    // fermato, quindi leve, ruote e cassetti non vanno frenati.
    player->SetNetworkLookGap(hasInput && repeated);
    if (mSmokeSeconds && hasInput && !mLoggedRemoteInput) {
        mLoggedRemoteInput = true;
        NetLog(std::string("[NET/TEST] Applying received input; player active=") + (active ? "yes" : "no"));
    }
    uint8_t buttons = active ? input.buttons : 0;
    mRemoteRotate = (buttons & 8) != 0;
    if (active) {
        cVector3f displacement = player->GetCharacterBody()->GetFeetPosition() - mRemoteSpawnFeet;
        if (mSmokeSeconds && OwnsRemotePlayer2() && !mObservedRemoteMovement &&
            (input.forward || input.strafe) && displacement.x * displacement.x + displacement.z * displacement.z > 0.01f) {
            mObservedRemoteMovement = true;
            NetLog("[NET/TEST] Remote Player 2 moved under network input");
        }
        auto apply = [&](const online::PlayerInput& command) {
            // Move(0) still marks the HPL body as moving and suppresses friction.
            // An idle axis must receive no Move call, as in the local input path.
            if(command.forward) player->Move(eCharDir_Forward, command.forward / 1000.0f);
            if(command.strafe) player->Move(eCharDir_Right, command.strafe / 1000.0f);
            const bool freshLook = !mHasLookSequence || command.sequence != mAppliedLookSequence;
            player->ApplyNetworkLookAngles(command.yaw / 1000.0f, command.pitch / 1000.0f,
                freshLook ? command.lookYaw / 10000.0f : 0, freshLook ? command.lookPitch / 10000.0f : 0);
            mAppliedLookSequence = command.sequence; mHasLookSequence = true;
            // Held keys, renewed every frame as in the local path. The lean's roll
            // and head offset reach the client's camera through the snapshot.
            if(command.buttons & 32) player->SetLean(1);
            if(command.buttons & 16) player->SetLean(-1);
        };
        // Dopo un blocco del client gli arretrati si recuperano: il comando più
        // vecchio viene integrato ora e il successivo dal passo fisico in
        // arrivo, così entrambi sono simulati e lo snapshot conferma il più
        // recente (come in Penumbra).
        online::PlayerInput extra;
        if(!LegacyInput() && !repeated && mSession.CatchUpInput(extra)) {
            apply(input);
            player->GetCharacterBody()->Update(gpBase->mpEngine->GetStepSize());
            input = extra; buttons = input.buttons; ++mInputCaughtUp;
        }
        apply(input);
    }
    if ((buttons ^ mPreviousButtons) & 1) player->Jump((buttons & 1) != 0);
    if ((buttons ^ mPreviousButtons) & 2) player->Crouch((buttons & 2) != 0);
    player->Run((buttons & 4) != 0);
    // Le azioni aspettano il comando di movimento in cui sono avvenute (vedi
    // ActionRequest::inputSequence), al più 0,5 s; nell'invio della 0.5.1
    // si applicano subito.
    { online::ActionRequest received; while(mSession.ReadAction(received)) mDeferredActions.push_back({received, std::chrono::steady_clock::now()}); }
    const auto actionNow = std::chrono::steady_clock::now();
    online::ActionRequest action;
    while(!mDeferredActions.empty()) {
        const auto& pending = mDeferredActions.front();
        const bool reached = LegacyInput() || !pending.request.inputSequence || !mHasLookSequence ||
            uint32_t(mAppliedLookSequence - pending.request.inputSequence) < 0x80000000u;
        if(!reached && actionNow - pending.received < std::chrono::milliseconds(500)) break;
        action = pending.request; mDeferredActions.pop_front();
        if(player->IsDead() || WaitingForMap() || SharedIntroView() || action.epoch != mEpoch) continue;
        // Inactive during a vision is intentional: accept only its page advance,
        // never ordinary interactions with the world while controls are locked.
        if(gpBase->mpEffectHandler->GetEmotionFlash()->IsActive() &&
            (action.action == online::PlayerAction::Interact || action.action == online::PlayerAction::Attack)) {
            gpBase->mpEffectHandler->GetEmotionFlash()->DoActionForPlayer(player,eLuxPlayerAction_Interact,action.pressed);
            if(action.pressed) {
                NetLog("[NET/EFFECT] Remote reader advanced authoritative emotion page");
                if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_EFFECTS")) mEffectsTestRemoteSkip = true;
            }
            continue;
        }
        if(!player->IsActive()) continue;
        if(action.action==online::PlayerAction::Jump) { player->Jump(action.pressed); continue; }
        if(action.action == online::PlayerAction::UseInventory || action.action == online::PlayerAction::CombineInventory) {
            gpBase->mpInventory->ApplyNetworkItemAction(player, action.item, action.secondItem, action.action == online::PlayerAction::CombineInventory);
            continue;
        }
        // The client supplies only an intent. Focus, reach, locks and object
        // ownership are checked by the host's existing player state.
        if(player->GetCurrentState() == eLuxPlayerState_Normal || player->GetCurrentState() == eLuxPlayerState_UseItem)
            player->GetCurrentStateData()->Update(0);
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_PICKUP") && action.action == online::PlayerAction::Interact && action.pressed) {
            cVector3f ray = player->GetInteractionForward();
            NetLog("[NET/TEST] Pickup ray=" + std::to_string(ray.x) + "," + std::to_string(ray.y) + "," + std::to_string(ray.z) +
                " focus=" + (player->GetEntityInFocus() ? player->GetEntityInFocus()->GetName() : "none"));
        }
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_LEVERS") && action.action == online::PlayerAction::Interact)
            NetLog("[NET/TEST] Host applies client interact pressed=" + std::to_string(action.pressed ? 1 : 0) +
                " focus=" + (player->GetEntityInFocus() ? player->GetEntityInFocus()->GetName() : "none") +
                " input=" + std::to_string(action.inputSequence) + " applied=" + std::to_string(mAppliedLookSequence) +
                " yaw=" + std::to_string(player->GetCamera()->GetYaw()) + " pitch=" + std::to_string(player->GetCamera()->GetPitch()));
        eLuxPlayerAction type = action.action == online::PlayerAction::Interact ? eLuxPlayerAction_Interact :
            action.action == online::PlayerAction::Lantern ? eLuxPlayerAction_Lantern :
            action.action == online::PlayerAction::Ignite ? eLuxPlayerAction_Ignite : eLuxPlayerAction_Attack;
        player->DoAction(type, action.pressed);
        if(action.action == online::PlayerAction::Lantern && action.pressed)
            NetLog("[NET/LANTERN] Remote intent applied: active=" + std::to_string(player->GetHelperLantern()->IsActive()) +
                " oil=" + std::to_string(player->GetLampOil()) + " item=" +
                std::to_string(gpBase->mpInventory->PlayerHasItemOfType(player,eLuxItemType_Lantern)));
        if(action.action == online::PlayerAction::Interact) mRemoteInteractHeld = action.pressed;
    }
    if(!active && mRemoteInteractHeld) { player->DoAction(eLuxPlayerAction_Interact, false); mRemoteInteractHeld = false; }
    mPreviousButtons = buttons;
    return true;
}
bool cLuxNetwork::KinematicPrediction() {
    static const bool kinematic = [] { const char* v = std::getenv("AMNESIA_NET_PREDICTION"); return v && std::string(v) == "kinematic"; }();
    return kinematic;
}
// AMNESIA_NET_LEGACY_INPUT=1 (su entrambi i PC): invio e consumo dei comandi
// come nella 0.5.1, solo per confronti A/B nella stessa build.
bool cLuxNetwork::LegacyInput() {
    static const bool legacy = [] { const char* v = std::getenv("AMNESIA_NET_LEGACY_INPUT"); return v && std::string(v) == "1"; }();
    return legacy;
}
// Client: correzioni della previsione per ogni nuova posa, ogni 5 s.
void cLuxNetwork::ReportPrediction(float error) {
    ++mCorrectionPoses; mCorrectionSum += error; mCorrectionMax = std::max(mCorrectionMax, error);
    if(error > 0.02f) ++mCorrectionsOver2cm;
    if(error > 0.10f) ++mCorrectionsOver10cm;
    const auto now = std::chrono::steady_clock::now();
    if(!mCorrectionReportAt.time_since_epoch().count()) mCorrectionReportAt = now;
    if(now - mCorrectionReportAt < std::chrono::seconds(5)) return;
    NetLog(std::string("[NET/PREDICTION] input=") + (LegacyInput() ? "legacy" : "queue") + " model=" + (KinematicPrediction() ? "kinematic" : "native") + " poses=" + std::to_string(mCorrectionPoses) +
        " over_2cm=" + std::to_string(mCorrectionsOver2cm) + " over_10cm=" + std::to_string(mCorrectionsOver10cm) +
        " mean_cm=" + std::to_string(mCorrectionPoses ? 100.0f * mCorrectionSum / mCorrectionPoses : 0.0f) +
        " max_cm=" + std::to_string(100.0f * mCorrectionMax) + " rtt_ms=" + std::to_string(mSession.Ping()));
    mCorrectionPoses = mCorrectionsOver2cm = mCorrectionsOver10cm = 0; mCorrectionSum = mCorrectionMax = 0; mCorrectionReportAt = now;
}
// Host: statistiche della coda dei comandi del client ogni 5 s.
void cLuxNetwork::ReportInputQueue(bool hasInput, bool repeated) {
    if(hasInput && !repeated) ++mInputNewTicks; else if(hasInput) ++mInputReusedTicks; else ++mInputIdleTicks;
    const auto now = std::chrono::steady_clock::now();
    if(!mInputQueueReportAt.time_since_epoch().count()) mInputQueueReportAt = now;
    if(now - mInputQueueReportAt < std::chrono::seconds(5)) return;
    const auto stats = mSession.TakeInputStats();
    if(mInputNewTicks || mInputReusedTicks)
        NetLog("[NET/INPUT] new_ticks=" + std::to_string(mInputNewTicks) + " reused_ticks=" + std::to_string(mInputReusedTicks) +
            " idle_ticks=" + std::to_string(mInputIdleTicks) + " starvations=" + std::to_string(stats.starvations) +
            " dropped=" + std::to_string(stats.dropped) + " lost=" + std::to_string(stats.lost) + " caught_up=" + std::to_string(mInputCaughtUp) +
            " queue_target=" + std::to_string(mSession.InputQueueTarget()) + " queue_depth=" + std::to_string(mSession.InputQueueDepth()) +
            " rtt_ms=" + std::to_string(mSession.Ping()));
    mInputNewTicks = mInputReusedTicks = mInputIdleTicks = mInputCaughtUp = 0; mInputQueueReportAt = now;
}
void cLuxNetwork::SendLocalAction(online::PlayerAction action, bool pressed) {
    if(!IsClientReplica() || !mReceivedSnapshot || !mEpoch || !mBaselineApplied || mClientSharedIntro) return;
    if(mClientDead && pressed) return;
    online::ActionRequest request; request.epoch = mEpoch; request.sequence = ++mActionSequence;
    request.inputSequence = mInputSequence + 1; // il comando di questo tick (o il successivo)
    request.action = action; request.pressed = pressed; mSession.SendAction(request);
}
void cLuxNetwork::SendInventoryAction(online::PlayerAction action, const std::string& item, const std::string& secondItem) {
    if(!IsClientReplica() || !mReceivedSnapshot || !mBaselineApplied || mClientSharedIntro) return;
    if(mClientDead) return;
    online::ActionRequest request; request.epoch = mEpoch; request.sequence = ++mActionSequence;
    request.inputSequence = mInputSequence + 1;
    request.action = action; request.pressed = true; request.item = item; request.secondItem = secondItem; mSession.SendAction(request);
}
void cLuxNetwork::OnGameStart() {
    if(mSession.GetRole() != online::Role::Host) return;
    // Runs after native OnStart/OnEnter, including Rainy Hall's SetLampOil(0).
    // Save load, reconnect and later map transitions do not invoke OnGameStart.
    gpBase->mpPlayer->SetLampOil(100);
    mFillRemoteStartingOil = gpBase->mpPlayer2 == NULL;
    if(gpBase->mpPlayer2) gpBase->mpPlayer2->SetLampOil(100);
    NetLog("[NET/LANTERN] New campaign host starts with oil=100");
}
void cLuxNetwork::OnMapEnter(cLuxMap*) {
    if(mSession.GetRole() == online::Role::Host) { mMapAnnounced = false; mPendingPresentation.clear(); }
}
void cLuxNetwork::QueuePresentation(online::Presentation event) {
    if(mSession.GetRole() != online::Role::Host || event.target == 1) return;
    event.epoch = 1; // The current map epoch is assigned only when the event is sent.
    if(online::EncodePresentation(event).empty()) { NetLog("[NET/STATE] Unsupported story event; closing session"); mSession.Stop(); return; }
    if(mPendingPresentation.size() >= 512) { NetLog("[NET/STATE] Presentation queue exceeded; closing session"); mSession.Stop(); return; }
    mPendingPresentation.push_back(std::move(event));
}
void cLuxNetwork::MusicChanged(int priority, float fadeTime) {
    if(mSession.GetRole() == online::Role::Host) mMusicFadeTimes[priority] = std::max(0.0f, fadeTime);
}
bool cLuxNetwork::WaitingForMap() const {
    if(mRecovering || mInputLeasePaused) return true;
    return mSession.GetRole() == online::Role::Host && mSession.Connected() &&
        mMapAnnounced && !mSession.IsWorldApplied(mEpoch);
}
bool cLuxNetwork::Host(uint16_t port) {
    ++mCheckpointContext;
    mEverConnected = mRecovering = false; mConnectionGeneration = 0;
    mConnectPort = port;
    mLastRole = online::Role::Host; gpBase->mpMainConfig->SetString("Online","LastRole","host");
    gpBase->mpMainConfig->SetString("Online","Port",std::to_string(port));
    mRecoveryCheckpoint.bytes.clear(); mMigrationAttempted = mMigrationHost = false;
    const bool started = mSession.StartHost(port);
    if(started) gpBase->mpEngine->SetWaitIfAppOutOfFocus(false);
    return started;
}
bool cLuxNetwork::Join(const std::string& address, uint16_t port) {
    ++mCheckpointContext;
    mEverConnected = mRecovering = false; mConnectionGeneration = 0;
    mConnectAddress = address; mConnectPort = port;
    mLastRole = online::Role::Client; gpBase->mpMainConfig->SetString("Online","LastRole","client");
    gpBase->mpMainConfig->SetString("Online","LastAddress",address);
    gpBase->mpMainConfig->SetString("Online","Port",std::to_string(port));
    mRecoveryCheckpoint.bytes.clear(); mMigrationAttempted = mMigrationHost = false;
    mSession.SetListenPort(port);
    const bool started = mSession.StartClient(address, port);
    if(started) gpBase->mpEngine->SetWaitIfAppOutOfFocus(false);
    return started;
}
void cLuxNetwork::SetClearImage(bool clear) {
    mClearImage = clear; gpBase->mpMainConfig->SetBool("Online","ClearImage",clear);
}
void cLuxNetwork::Disconnect(bool menuHandlesReset) {
    if(mSession.GetRole() != online::Role::Offline) {
        mLastRole = mSession.GetRole();
        gpBase->mpMainConfig->SetString("Online","LastRole",mLastRole == online::Role::Host ? "host" : "client");
        gpBase->mpMainConfig->SetString("Online","Port",std::to_string(mConnectPort));
        if(mLastRole == online::Role::Client && !mConnectAddress.empty())
            gpBase->mpMainConfig->SetString("Online","LastAddress",mConnectAddress);
    }
    ++mCheckpointContext; mSession.CloseVoluntarily(); mRecovering = mEverConnected = mClientSharedIntro = false;
    mInputLeaseSeen = mInputLeasePaused = false; mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0;
    if(menuHandlesReset) mWasClient = mHadClientConnection = false;
}
bool cLuxNetwork::Reconnect() {
    const auto role = mSession.GetRole() == online::Role::Offline ? mLastRole : mSession.GetRole();
    if(role == online::Role::Offline) return false;
    uint16_t port = mConnectPort;
    if(mSession.GetRole() == online::Role::Offline && !Port(gpBase->mpMainConfig->GetString("Online","Port","27020"),port)) return false;
    const auto address = mSession.GetRole() == online::Role::Client ? mConnectAddress :
        gpBase->mpMainConfig->GetString("Online","LastAddress","127.0.0.1");
    const bool inGame = gpBase->mpMapHandler->MapIsLoaded();
    ++mCheckpointContext; mConnectionGeneration = 0; mInputLeaseSeen = mInputLeasePaused = false;
    mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0; mMigrationAttempted = mMigrationHost = false;
    mRecovering = mEverConnected = inGame;
    mRecoveryStart = mRetryAt = std::chrono::steady_clock::now();
    mConnectAddress = address; mConnectPort = port;
    mSession.SetListenPort(port);
    const bool ok = role == online::Role::Host ? mSession.StartHost(port,true) : mSession.StartClient(address,port,true);
    if(ok) { gpBase->mpEngine->SetWaitIfAppOutOfFocus(false); NetLog("[NET/CONNECTION] Manual reconnect requested"); }
    else mRecovering = mEverConnected = false;
    return ok;
}
std::string cLuxNetwork::ConnectionStatus() const {
    if(mRecovering && mSession.GetRole() == online::Role::Host && mSession.PartnerLeftVoluntarily())
        return "Il partner e uscito. Campagna in pausa, in attesa che si ricolleghi.";
    if(mInputLeasePaused) return "La rete non risponde: campagna in pausa, attesa del partner.";
    if(mMigrationHost && mRecovering) return "Host trasferito a questo PC. Campagna recuperata, in attesa del partner.";
    if(mRecovering) return mSession.RetryAllowed() ? "Connessione persa: riconnessione automatica, campagna in pausa." : "Connessione rifiutata: controlla versione e dati del gioco.";
    if(mSession.GetRole() == online::Role::Offline) return "Scegli Ospita oppure inserisci l'IP del tuo amico e premi Connetti.";
    if(!mSession.Connected()) return mSession.GetRole() == online::Role::Host ? "In attesa del tuo amico sulla porta UDP " + std::to_string(mConnectPort) : "Connessione in corso...";
    if(!mSession.LobbyReady()) return "Amico connesso. Entrambi devono premere Pronto.";
    return "Entrambi pronti. L'host puo avviare la campagna. Ping: " + std::to_string(mSession.Ping()) + " ms";
}
void cLuxNetwork::LoadReplicaMap(const online::MapState& state) {
    mPropMotion.clear(); mEnemyMotion.clear(); mHasLastLocalFeet = false;
    if(MapFingerprint(state.file) != state.fingerprint) {
        NetLog("[NET/MAP] Map/script content mismatch; both PCs need identical game files");
        mSession.Stop(); return;
    }
    gpBase->mpEffectHandler->GetPlayVoice()->StopVoices(1);
    // Never load from inside ENet's receive loop: map setup broadcasts Reset.
    if(gpBase->mpUserConfig == NULL) {
        gpBase->CreateProfile(gpBase->msDefaultProfileName);
        gpBase->SetProfile(gpBase->msDefaultProfileName);
        gpBase->InitUserConfig();
    }
    gpBase->mpEngine->GetUpdater()->SetContainer("Default");
    gpBase->mpInputHandler->ChangeState(eLuxInputState_Game);
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_CAMPAIGN_START")) {
        NetLog("[NET/TEST] Simulating six-second synchronous client map load");
        std::this_thread::sleep_for(std::chrono::seconds(6));
    }
    const size_t slash = state.file.find_last_of('/');
    if(!gpBase->mpMapHandler->GetCurrentMap()) {
        gpBase->StartGame(state.file.substr(slash + 1), state.file.substr(0, slash + 1), "");
    } else {
        cLuxMap* old = gpBase->mpMapHandler->GetCurrentMap();
        gpBase->mpMapHandler->SetMapFolder(state.file.substr(0, slash + 1));
        cLuxMap* next = gpBase->mpMapHandler->LoadMap(state.file.substr(slash + 1), true);
        gpBase->mpMapHandler->DestroyMap(old, false);
        gpBase->mpMapHandler->SetCurrentMap(next, false, false, "");
        mReplicaIDs.clear(); mBaselineApplied = false; mAppliedInventory = false; mAppliedJournal = false;
        mLoggedWorldReplica = false; mLoggedLighting = false;
    }
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(!map || !gpBase->mpPlayer->GetCharacterBody()) {
        NetLog("[NET/MAP] Replica map load failed; closing session"); mSession.Stop(); return;
    }
    mEpoch = state.epoch;
    mReplicaMediaIDs.clear(); mMediaStates.clear(); mMediaBaselineApplied = false;
    gpBase->mpMusicHandler->Reset();
    gpBase->mpEngine->GetSound()->GetMusicHandler()->Stop(1000);
    mSnapshots.clear(); mReceivedSnapshot = false; mLastSnapshotAt = {};
    mHasClientLife = false; mClientDead = mClientSharedIntro = false;
    mPendingLookYaw = mPendingLookPitch = 0; mClientObjectLook = false;
    map->GetWorld()->SetPhysicsSimulationEnabled(false);
    // Match the host's literal OnEnter preloads during the loading phase, before
    // announcing readiness, so first-use effects need less disk/decoder work.
    PreloadReplicaAssets(state.file);
    if(!mSession.SendMapReady(mEpoch)) { NetLog("[NET/MAP] Cannot send map readiness; closing session"); mSession.Stop(); return; }
    NetLog("[NET/MAP] Replica loaded epoch=" + std::to_string(mEpoch) + " file=" + state.file);
}
void cLuxNetwork::ApplyReplica(cLuxPlayer* player, const online::PlayerState& state, bool localView) {
    if(!player || !player->GetCharacterBody()) return;
    iCharacterBody* body = player->GetCharacterBody();
    cVector3f position(state.x / 1000.0f, state.y / 1000.0f, state.z / 1000.0f);
    cVector3f displacement = position - body->GetFeetPosition();
    const float step = gpBase->mpEngine->GetStepSize();
    if(player->GetAvatar()) player->GetAvatar()->SetNetworkVelocity(step > 0 && displacement.Length() < 5 ? displacement / step : cVector3f(0));
    body->SetActiveSize((state.flags & 1) ? 1 : 0);
    body->SetFeetPosition(position);
    if(localView) { mClientObjectLook = (state.flags & 32) != 0; mClientScriptLook = (state.flags & 80) != 0; }
    const bool localLook = localView && (state.flags & 4) && !(state.flags & 48);
    body->SetYaw(localLook ? player->GetCamera()->GetYaw() : state.yaw / 1000.0f);
    if(!localLook) {
        player->GetCamera()->SetYaw(state.yaw / 1000.0f);
        player->GetCamera()->SetPitch(state.pitch / 1000.0f);
    }
    // Physics is disabled on replicas; position the camera without advancing a body.
    player->GetCamera()->SetPosition(body->GetFeetPosition() + cVector3f(state.cameraX / 1000.0f, state.eyeHeight / 1000.0f, state.cameraZ / 1000.0f));
    player->GetCamera()->SetRoll(state.roll / 1000.0f);
    player->GetCamera()->SetFOV(state.fov / 1000.0f);
    // The client keeps its own display ratio; reproduce only the scripted multiplier.
    const float displayAspect = gpBase->mpEngine->GetGraphics()->GetLowLevel()->GetScreenSizeFloat().x /
        gpBase->mpEngine->GetGraphics()->GetLowLevel()->GetScreenSizeFloat().y;
    player->GetCamera()->SetAspect(displayAspect * state.aspect / 1000.0f);
    player->SetLookSpeedMul(state.lookSpeed / 1000.0f);
    player->ApplyNetworkVitals(state.health / 100.0f, state.sanity / 100.0f, state.oil / 100.0f);
    player->SetTinderboxes(state.tinderboxes);
    player->SetActive((state.flags & 4) != 0);
    player->GetHelperLantern()->SetActive((state.flags & 2) != 0, false, false, false);
}
void cLuxNetwork::UpdateReplication(float timeStep) {
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    const auto now = std::chrono::steady_clock::now();
    if(!mSession.Connected()) {
        mSendingCheckpoint.bytes.clear(); mLastCheckpointEpoch = 0;
        mMapAnnounced = false; mHostMapFile.clear();
        mSnapshots.clear(); mReceivedSnapshot = false; mLastSnapshotAt = {};
        mHasClientLife = mHasHostLife = false; mClientDead = false;
        if(map) map->GetWorld()->SetPhysicsSimulationEnabled(!IsClientReplica() && !mRecovering);
        return;
    }
    if(IsClientReplica()) {
        online::MapState state;
        if(mSession.ReadMap(state) && (state.epoch != mEpoch || !map)) {
            LoadReplicaMap(state);
            map = gpBase->mpMapHandler->GetCurrentMap();
        }
        if(!map) return;
        map->GetWorld()->SetPhysicsSimulationEnabled(false);
        // Consume transitions separately from interpolation. Even if death and
        // revival arrive in the same ENet poll, show a complete death fade.
        online::Snapshot life;
        if((!mClientDead || now - mClientDeathStart >= std::chrono::milliseconds(400)) && mSession.ReadLife(life)) {
            if(life.epoch == mEpoch) {
                const bool dead = (life.players[1].flags & 8) != 0;
                if(dead != mClientDead) {
                    mClientDead = dead;
                    if(dead) { mClientDeathStart = now; gpBase->mpEffectHandler->GetFade()->FadeOut(0.35f); }
                    else gpBase->mpEffectHandler->GetFade()->FadeIn(0.5f);
                    if(mSmokeSeconds) NetLog(dead ? "[NET/TEST] Authoritative death applied on client" : "[NET/TEST] Authoritative checkpoint revival applied on client");
                }
                mClientLife = life; mHasClientLife = true;
                {
                    if(!mReceivedSnapshot) {
                        NetLog("[NET/STATE] First authoritative player snapshot applied");
                        const auto& p = life.players[1]; mReplicaInitialFeet = cVector3f(p.x / 1000.0f, p.y / 1000.0f, p.z / 1000.0f);
                    }
                    online::Snapshot pose = life, latest;
                    if(mSession.ReadSnapshot(latest) && latest.epoch == mEpoch &&
                        latest.sequence - life.sequence != 0 && latest.sequence - life.sequence < 0x80000000u) pose = latest;
                    // A checkpoint is a discontinuity even within five metres.
                    mSnapshots.clear(); mSnapshots.push_back({pose, now});
                    mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0;
                    mReceivedSequence = pose.sequence; mReceivedSnapshot = true;
                }
            }
        }
        online::MediaState media;
        bool mediaIDsReady = false;
        auto batchStart = std::chrono::steady_clock::now();
        for(int count = 0; count < 128 && mSession.ReadMedia(media); ++count) {
            if(!mediaIDsReady) { EnsureMediaIDs(map->GetWorld(),mNextMediaID); mediaIDsReady = true; }
            const auto start = std::chrono::steady_clock::now();
            if(media.epoch == mEpoch) ApplyMedia(map, media);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            if(us >= 2000) NetLog("[NET/PERF] Replica media kind=" + std::to_string(unsigned(media.kind)) + " item_us=" + std::to_string(us) + " file=" + media.file);
            if(std::chrono::steady_clock::now()-batchStart >= std::chrono::microseconds(750)) break;
        }
        online::Presentation event;
        batchStart = std::chrono::steady_clock::now();
        for(int count = 0; count < 128 && mSession.ReadPresentation(event); ++count) {
            const auto start = std::chrono::steady_clock::now();
            if(event.epoch == mEpoch && event.target != 1) ApplyPresentation(map, event);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            if(us >= 2000) NetLog("[NET/PERF] Replica presentation kind=" + std::to_string(unsigned(event.kind)) + " item_us=" + std::to_string(us) + " asset=" + event.text[0]);
            if(std::chrono::steady_clock::now()-batchStart >= std::chrono::microseconds(750)) break;
        }
        online::EntityState entity;
        batchStart = std::chrono::steady_clock::now();
        for(int count = 0; count < 128 && mSession.ReadEntity(entity); ++count) {
            const auto start = std::chrono::steady_clock::now();
            if(entity.epoch == mEpoch) ApplyWorldEntity(map, entity);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            if(us >= 2000) NetLog("[NET/PERF] Replica entity item_us=" + std::to_string(us) + " name=" + entity.name);
            if(std::chrono::steady_clock::now()-batchStart >= std::chrono::microseconds(750)) break;
        }
        if(!mSession.Connected()) return;
        UpdatePropMotion(map,timeStep);
        UpdateEnemyMotion(map,timeStep);
        if(!mBaselineApplied && mSession.IsWorldReady(mEpoch)) {
            // Remove map-authored audio/particles absent from the host's roster.
            cWorld* world = map->GetWorld();
            std::map<int, bool> soundIDs, particleIDs;
            for(const auto& pair : mReplicaMediaIDs) {
                if(pair.first.first == uint8_t(online::MediaKind::Sound)) soundIDs[pair.second] = true;
                if(pair.first.first == uint8_t(online::MediaKind::Particle)) particleIDs[pair.second] = true;
            }
            std::vector<cSoundEntity*> sounds; std::vector<cParticleSystem*> particles;
            auto soundsIt = world->GetSoundEntityIterator();
            while(soundsIt.HasNext()) { auto* sound = soundsIt.Next(); if(!soundIDs.count(sound->GetUniqueID())) sounds.push_back(sound); }
            auto particlesIt = world->GetParticleSystemIterator();
            while(particlesIt.HasNext()) { auto* ps = particlesIt.Next(); if(!particleIDs.count(ps->GetUniqueID())) particles.push_back(ps); }
            for(auto* sound : sounds) world->DestroySoundEntity(sound);
            for(auto* ps : particles) world->DestroyParticleSystem(ps);
            mMediaBaselineApplied = true;
            // OnStart may remove original map entities on the host. The client
            // does not run that script, so reconcile the complete host roster.
            std::map<int, bool> roster;
            for(const auto& pair : mReplicaIDs) roster[pair.second] = true;
            cLuxEntityIterator it = map->GetEntityIterator();
            while(it.HasNext()) {
                iLuxEntity* original = it.Next();
                if(original->GetEntityType() <= eLuxEntityType_Enemy && roster.find(original->GetID()) == roster.end()) map->DestroyEntity(original);
            }
            mBaselineApplied = true;
            NetLog("[NET/WORLD] Complete host entity baseline applied");
        }
        map->FlushNetworkDestruction();
        online::InventoryState inventory;
        online::JournalState journal;
        if(mSession.ReadJournal(journal) && journal.epoch == mEpoch &&
            (!mAppliedJournal || journal.revision != mAppliedJournalRevision) && gpBase->mpJournal->ApplyNetworkJournal(journal)) {
            if(!mAppliedJournal) NetLog("[NET/JOURNAL] Complete journal history applied");
            mAppliedJournal = true; mAppliedJournalRevision = journal.revision;
        }
        if(mSession.ReadInventory(inventory) && inventory.epoch == mEpoch && (!mAppliedInventory || inventory.revision != mAppliedInventoryRevision)) {
            gpBase->mpInventory->ApplyNetworkInventory(inventory);
            if(!mAppliedInventory) NetLog("[NET/INVENTORY] Shared host inventory applied");
            mAppliedInventory = true; mAppliedInventoryRevision = inventory.revision;
        }
        online::Snapshot snapshot;
        if(mSession.ReadSnapshot(snapshot) && (!mReceivedSnapshot ||
            (snapshot.sequence - mReceivedSequence != 0 && snapshot.sequence - mReceivedSequence < 0x80000000u))) {
            if(mLastSnapshotAt.time_since_epoch().count()) {
                const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastSnapshotAt).count();
                mSnapshotGapTotalMs += gap; mSnapshotGapMaxMs = std::max(mSnapshotGapMaxMs,int64_t(gap)); ++mSnapshotGapSamples;
                if(gap > 250) NetLog("[NET/DIAG] Authoritative snapshot gap=" + std::to_string(gap) +
                    " ms epoch=" + std::to_string(mEpoch) + " rtt_ms=" + std::to_string(mSession.Ping()));
            }
            mLastSnapshotAt = now;
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_ENEMY") && !mTestEnemyDamage &&
                (snapshot.players[0].health < 9900 || snapshot.players[1].health < 9900)) {
                mTestEnemyDamage = true; NetLog("[NET/TEST] Host enemy damage applied on client");
            }
            if(!mReceivedSnapshot) {
                NetLog("[NET/STATE] First authoritative player snapshot applied");
                const auto& p = snapshot.players[1]; mReplicaInitialFeet = cVector3f(p.x / 1000.0f, p.y / 1000.0f, p.z / 1000.0f);
            }
            mReceivedSequence = snapshot.sequence; mReceivedSnapshot = true;
            mSnapshots.push_back({snapshot, now});
            if(mSnapshots.size() > 32) mSnapshots.pop_front();
        }
        if(mSnapshots.empty()) return;
        const auto renderTime = now - std::chrono::milliseconds(100);
        while(mSnapshots.size() > 2 && mSnapshots[1].received <= renderTime) mSnapshots.pop_front();
        online::Snapshot interpolated = mSnapshots.front().state;
        if(mSnapshots.size() > 1) {
            const auto& a = mSnapshots[0]; const auto& b = mSnapshots[1];
            float duration = std::chrono::duration<float>(b.received - a.received).count();
            float alpha = duration > 0 ? std::chrono::duration<float>(renderTime - a.received).count() / duration : 1;
            alpha = std::max(0.0f, std::min(1.0f, alpha));
            for(int i = 0; i < 2; ++i) {
                auto& p = interpolated.players[i]; const auto& q = b.state.players[i];
                // Teleports/checkpoints snap rather than crossing walls.
                const double dx = double(q.x) - p.x, dy = double(q.y) - p.y, dz = double(q.z) - p.z;
                if(dx*dx + dy*dy + dz*dz > 25000000.0) { p = q; continue; }
                p.x += int32_t((q.x - p.x) * alpha); p.y += int32_t((q.y - p.y) * alpha); p.z += int32_t((q.z - p.z) * alpha);
                float delta = (q.yaw - p.yaw) / 1000.0f;
                float yaw = p.yaw / 1000.0f + std::atan2(std::sin(delta), std::cos(delta)) * alpha;
                p.yaw = int16_t(std::atan2(std::sin(yaw), std::cos(yaw)) * 1000);
                p.pitch += int16_t((q.pitch - p.pitch) * alpha);
                p.health = q.health; p.sanity = q.sanity; p.oil = q.oil;
                p.eyeHeight += int16_t((q.eyeHeight - p.eyeHeight) * alpha);
                p.flags = q.flags; p.tinderboxes = q.tinderboxes;
                p.roll = q.roll; p.fov = q.fov; p.aspect = q.aspect; p.lookSpeed = q.lookSpeed;
                p.cameraX += int16_t((q.cameraX - p.cameraX) * alpha);
                p.cameraZ += int16_t((q.cameraZ - p.cameraZ) * alpha);
            }
        }
        // The local viewport stays P1 internally; network ownership is explicit.
        if(mHasClientLife) for(int i = 0; i < 2; ++i) {
            interpolated.players[i].flags = (interpolated.players[i].flags & ~12) | (mClientLife.players[i].flags & 12);
            if(mClientLife.players[i].flags & 8) interpolated.players[i].health = 0;
        }
        if(mClientDead) interpolated.players[1] = mClientLife.players[1];
        online::PlayerState local = mSnapshots.back().state.players[1];
        if(mClientDead) local = mClientLife.players[1];
        auto* body = gpBase->mpPlayer->GetCharacterBody();
        if(mBaselineApplied && mReceivedSnapshot && mAppliedInventory && mAppliedJournal &&
            mSession.IsWorldReady(mEpoch) && !mSession.IsWorldApplied(mEpoch)) {
            if(!mSession.SendWorldApplied(mEpoch)) { NetLog("[NET/MAP] Cannot confirm applied world; closing session"); mSession.Stop(); return; }
            NetLog("[NET/MAP] Full replica world acknowledged epoch=" + std::to_string(mEpoch));
        }
        mClientSharedIntro = mSession.IsWorldApplied(mEpoch) && (local.flags & 64) != 0;
        if(mClientSharedIntro) { mPrediction.Clear(); mVerticalPrediction.Clear(); mLocalInputButtons=0; }
        if(body && mBaselineApplied && !mClientSharedIntro) {
            const auto predictionStart = std::chrono::steady_clock::now();
            online::PredictionCollisionCache collisionCache;
            auto collide = [&](online::PredictedPosition from, online::PredictedPosition to) {
                auto query = [&](float x,float z) {
                    return online::StepBlocked(local.y/1000.0f,body->GetMaxStepSize(),local.grounded && !mVerticalPrediction.Pending(),[&](float feet) {
                        cVector3f push;
                        map->GetPhysicsWorld()->CheckShapeWorldCollision(&push,body->GetCurrentShape(),cMath::MatrixTranslate(cVector3f(x,feet+body->GetCurrentShape()->GetSize().y/2,z)),body->GetCurrentBody(),false,true,NULL,true,0,eFlagBit_All,false,body->GetIgnoreBody());
                        return online::PredictionContact{push.x,push.y,push.z};
                    });
                };
                // Native-sized steps remain reachable; walls still slide.
                online::PredictedPosition result = from;
                if(to.x != from.x && !collisionCache.Blocked(to.x,from.z,query)) result.x = to.x;
                if(to.z != from.z && !collisionCache.Blocked(result.x,to.z,query)) result.z = to.z;
                return result;
            };
            // Previsione con il corpo del motore (come sull'host); il modello
            // cinematico resta per i confronti (AMNESIA_NET_PREDICTION=kinematic).
            NativeStepper stepper(body);
            const auto predicted = KinematicPrediction() ?
                mPrediction.ReconcileSmoothed(local,mSnapshots.back().state.acknowledgedInput,mSnapshots.back().state.sequence,timeStep,collide) :
                mPrediction.ReconcileSmoothedWith(local,mSnapshots.back().state.acknowledgedInput,mSnapshots.back().state.sequence,timeStep,stepper,collide);
            { float error = 0; if(mPrediction.TakePoseCorrection(error)) ReportPrediction(error); }
            const auto predictionUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-predictionStart).count();
            if(predictionUs >= 5000) NetLog("[NET/PERF] Replica prediction item_us=" + std::to_string(predictionUs) +
                " collision_queries=" + std::to_string(collisionCache.Queries()) + " reused=" + std::to_string(collisionCache.Hits()));
            float groundHeight=local.y/1000.0f;
            if(local.grounded && !mVerticalPrediction.Pending() &&
               (std::fabs(predicted.x-local.x/1000.0f)+std::fabs(predicted.z-local.z/1000.0f)>.001f)) {
                groundHeight=online::StepSupport(groundHeight,body->GetMaxStepSize(),[&](float feet) {
                    cVector3f push;
                    map->GetPhysicsWorld()->CheckShapeWorldCollision(&push,body->GetCurrentShape(),cMath::MatrixTranslate(cVector3f(predicted.x,feet+body->GetCurrentShape()->GetSize().y/2,predicted.z)),body->GetCurrentBody(),false,true,NULL,true,0,eFlagBit_All,false,body->GetIgnoreBody());
                    return online::PredictionContact{push.x,push.y,push.z};
                });
            }
            local.x = int32_t(predicted.x*1000); local.z = int32_t(predicted.z*1000);
            const float poseAge=std::chrono::duration<float>(now-mSnapshots.back().received).count();
            const float y=mVerticalPrediction.Update(local,mSnapshots.back().state.acknowledgedInput,mSnapshots.back().state.sequence,timeStep,poseAge,[&](float from,float to) {
                const float distance=std::fabs(to-from);
                if(distance<.00001f) return to;
                if(distance>1.28f) return from;
                // Sweep in small increments so a predicted rise cannot cross a
                // low ceiling and a predicted landing cannot cross the floor.
                const int steps=std::min(32,std::max(1,int(std::ceil(std::fabs(to-from)/.04f))));
                float safe=from;
                for(int i=1;i<=steps;++i) {
                    const float candidate=from+(to-from)*i/steps; cVector3f push;
                    const cVector3f center(local.x/1000.0f,candidate+body->GetCurrentShape()->GetSize().y/2,local.z/1000.0f);
                    const bool blocked=map->GetPhysicsWorld()->CheckShapeWorldCollision(&push,body->GetCurrentShape(),cMath::MatrixTranslate(center),body->GetCurrentBody(),false,true,NULL,true,0,eFlagBit_All,false,body->GetIgnoreBody());
                    if(blocked && ((to>from && push.y<-.001f) || (to<from && push.y>.001f && (!local.grounded || mVerticalPrediction.Pending())))) break;
                    safe=candidate;
                }
                return safe;
            },groundHeight);
            local.y=int32_t(y*1000);
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_PREDICTION") && !mPredictionSmoothTestObserved && mPrediction.CorrectionDistance() > 0.002f) {
                mPredictionSmoothTestObserved = true; NetLog("[NET/TEST] Small authoritative movement correction smoothed with collision checks");
            }
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_PREDICTION") && !mPredictionTestObserved && mPrediction.Pending() &&
                (std::abs(local.x-mSnapshots.back().state.players[1].x) + std::abs(local.z-mSnapshots.back().state.players[1].z) > 5)) {
                mPredictionTestObserved = true; NetLog("[NET/TEST] Local movement prediction applied before host acknowledgement with world collision checks");
            }
        }
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA") && !mTestLanternDisabled) {
            // Reproduce stale local save permissions. Only the host may veto use.
            gpBase->mpPlayer->GetHelperLantern()->SetDisabled(true);
            gpBase->mpPlayer2->GetHelperLantern()->SetDisabled(true);
            mTestLanternDisabled = true;
            NetLog("[NET/TEST] Injected stale local lantern permissions on client");
        }
        ApplyReplica(gpBase->mpPlayer, local, true);
        DiagnoseLocalReplica(local, now);
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_SNAPSHOT_BLACKOUT") && !mTestSnapshotBackupLogged &&
            mSession.SnapshotBackupsReceived() >= 15) {
            mTestSnapshotBackupLogged = true;
            NetLog("[NET/TEST] Reliable snapshot backups applied without life reset");
        }
        ApplyReplica(gpBase->mpPlayer2, interpolated.players[0], false);
        if(mClientSharedIntro) {
            // The original intro belongs to Daniel's camera. Keep the remote
            // player's body separate, but share the scripted view until wake-up.
            auto* camera = gpBase->mpPlayer->GetCamera();
            auto* original = gpBase->mpPlayer2->GetCamera();
            camera->SetPosition(original->GetPosition()); camera->SetYaw(original->GetYaw()); camera->SetPitch(original->GetPitch());
            camera->SetRoll(original->GetRoll()); camera->SetFOV(original->GetFOV()); camera->SetAspect(original->GetAspect());
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_CAMPAIGN_START") && !mIntroCameraTestObserved) {
                mIntroCameraTestObserved = true; NetLog("[NET/TEST] Client shares original intro camera with host; companion avatars suppressed");
            }
        }
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA")) {
            const float elapsed = std::chrono::duration<float>(now - mSmokeStart).count();
            if(!mTestLanternRequested && elapsed > 5 && mBaselineApplied && mAppliedInventory &&
                gpBase->mpInventory->GetItem("online_lantern_fixture") && gpBase->mpPlayer->GetLampOil() > 20 &&
                gpBase->mpPlayer->GetLampOil() < 50) {
                SendLocalAction(online::PlayerAction::Lantern, true);
                SendLocalAction(online::PlayerAction::Lantern, false);
                mTestLanternRequested = true;
            }
            auto lit = [](cLuxPlayer* player) {
                auto* lantern = player->GetHelperLantern(); auto* light = lantern->GetLight();
                return lantern->IsActive() && light && light->GetDiffuseColor().r > 0.01f;
            };
            if(!mTestLanternRemoteLogged && lit(gpBase->mpPlayer2)) {
                mTestLanternRemoteLogged = true; NetLog("[NET/TEST] Remote host lantern emits light on client");
            }
            if(!mTestLanternLocalLogged && lit(gpBase->mpPlayer) && lit(gpBase->mpPlayer2) &&
                gpBase->mpPlayer2->GetLampOil() - gpBase->mpPlayer->GetLampOil() > 10) {
                mTestLanternLocalLogged = true; NetLog("[NET/TEST] Both lanterns emit light with independent oil on client");
            }
            auto* hands = gpBase->mpPlayer->GetHands();
            auto* held = hands ? hands->GetCurrentHandObject() : NULL;
            auto* handMesh = hands ? hands->GetHandsEntity() : NULL;
            if(!mTestLanternHandsLogged && mClientMoved && lit(gpBase->mpPlayer) && handMesh && held &&
                held->GetName() == "lantern" && held->GetMeshEntity() && held->GetMeshEntity()->IsActive() &&
                (handMesh->GetWorldPosition() - gpBase->mpPlayer->GetCamera()->GetPosition()).Length() < 0.25f) {
                mTestLanternHandsLogged = true;
                NetLog("[NET/TEST] Client held lantern follows moving camera with stale local permissions");
            }
            if(!mTestLanternIndependentLogged && mTestLanternLocalLogged && elapsed > 12 && elapsed < 18 &&
                !gpBase->mpPlayer2->GetHelperLantern()->IsActive() && lit(gpBase->mpPlayer)) {
                mTestLanternIndependentLogged = true; NetLog("[NET/TEST] Independent lantern toggles applied on client");
            }
            if(!mTestLanternAudioBaseline && mEpoch == 2 && mBaselineApplied && elapsed > 11 &&
                mTestLanternOnSounds == 1 && mTestLanternOffSounds == 0) {
                mTestLanternAudioBaseline = true;
                NetLog("[NET/TEST] Lantern save/load baseline and repeated snapshots stayed silent");
            }
            if(!mTestLanternOffRequested && mTestLanternAudioBaseline && mTestLanternIndependentLogged) {
                SendLocalAction(online::PlayerAction::Lantern, true); SendLocalAction(online::PlayerAction::Lantern, false);
                mTestLanternOffRequested = true;
            }
            if(mTestLanternOffRequested && !mTestLanternOnRequested && !lit(gpBase->mpPlayer) && mTestLanternOffSounds == 1) {
                SendLocalAction(online::PlayerAction::Lantern, true); SendLocalAction(online::PlayerAction::Lantern, false);
                mTestLanternOnRequested = true;
            }
            if(!mTestLanternAudioToggles && mTestLanternOnRequested && lit(gpBase->mpPlayer) &&
                mTestLanternOnSounds == 2 && mTestLanternOffSounds == 1) {
                mTestLanternAudioToggles = true;
                NetLog("[NET/TEST] Client lantern on/off/on produced exactly one audible event per toggle");
            }
            if(!mTestLanternEmptyRequested && mTestLanternAudioToggles && elapsed > 43 &&
                gpBase->mpPlayer->GetLampOil() == 0 && !lit(gpBase->mpPlayer) && mTestLanternOffSounds == 2) {
                SendLocalAction(online::PlayerAction::Lantern, true); SendLocalAction(online::PlayerAction::Lantern, false);
                mTestLanternEmptyRequested = true;
            }
            if(!mTestLanternAudioCounts && mTestLanternEmptyRequested && elapsed > 50 &&
                mTestLanternOnSounds == 2 && mTestLanternOffSounds == 3 && !lit(gpBase->mpPlayer)) {
                mTestLanternAudioCounts = true;
                NetLog("[NET/TEST] Lantern audio one-shot counts verified on=2 off=3 including depletion and empty attempt");
            }
        }
        if(mSmokeSeconds && !mClientMoved && gpBase->mpPlayer->GetCharacterBody()) {
            cVector3f delta = gpBase->mpPlayer->GetCharacterBody()->GetFeetPosition() - mReplicaInitialFeet;
            if(delta.x*delta.x + delta.z*delta.z > 0.01f) { mClientMoved = true; NetLog("[NET/TEST] Client camera/body moved from authoritative snapshots"); }
        }
        return;
    }
    if(!map || !gpBase->mpPlayer->GetCharacterBody()) return;
    if(!mMapAnnounced || mHostMapFile != map->GetFileName()) {
        online::MapState state; state.epoch = ++mEpoch; state.file = map->GetFileName();
        std::replace(state.file.begin(), state.file.end(), '\\', '/');
        state.fingerprint = MapFingerprint(state.file);
        if(!mSession.SendMap(state)) {
            NetLog("[NET/MAP] Cannot announce map file=" + state.file + " fingerprint=" + std::to_string(state.fingerprint) + "; closing session");
            mSession.Stop(); return;
        }
        mHostMapFile = map->GetFileName(); mMapAnnounced = true; mMapWaitStart = now;
        // The client cannot send input for the new epoch until its baseline is
        // applied. Start the input lease again after the first new input.
        mInputLeaseSeen = mInputLeasePaused = false;
        mSendingCheckpoint.bytes.clear(); mLastCheckpointEpoch = 0;
        mEntityBytes.clear(); mEntityStates.clear(); mEntityMotionAt.clear(); mEntityMotionSettled.clear(); mEntitySentAt.clear();
        mBaselineSent = false;
        mHasHostLife = false; mSnapshotAccumulator = mSnapshotBackupAccumulator = mWorldReplicationAccumulator = 0;
        mHasLookSequence = false;
        mInventoryBytes.clear();
        mJournalBytes.clear();
        mLightBytes.clear();
        mMediaBytes.clear(); mMediaStates.clear();
        for(auto& bytes : mGlobalAudioBytes) bytes.clear();
        for(auto& bytes : mFogBytes) bytes.clear();
        NetLog("[NET/MAP] Announced epoch=" + std::to_string(mEpoch) + " file=" + state.file);
    }
    map->GetWorld()->SetPhysicsSimulationEnabled(!WaitingForMap());
    // Native scripts/physics wait for application of the full client world.
    // Replication must continue between MapReady and WorldApplied, otherwise
    // the baseline needed to release that barrier would never be sent.
    // Session owns the bounded loading timeout and retains its host listener on recovery.
    if(mRecovering || mInputLeasePaused || !mSession.IsMapReady(mEpoch)) return;
    if(!gpBase->mpPlayer2 || !gpBase->mpPlayer2->GetCharacterBody()) return;
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA")) UpdateMediaTest(map, std::chrono::duration<float>(now - mSmokeStart).count());
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_ENEMY")) {
        if(!mTestEnemySpawned) {
            cLuxPlayer* player = gpBase->mpPlayer2;
            cVector3f position = player->GetCharacterBody()->GetFeetPosition() + player->GetCamera()->GetForward() * 3.0f;
            map->CreateEntity("online_test_grunt", "entities/enemy/servant_grunt/servant_grunt.ent", cMath::MatrixTranslate(position), cVector3f(1));
            iLuxEntity* entity = map->GetEntityByName("online_test_grunt");
            if(entity && entity->GetEntityType() == eLuxEntityType_Enemy) {
                iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
                enemy->SetActive(true); enemy->ChangeState(eLuxEnemyState_Wait); enemy->SetDisabled(true);
                if(!enemy->TestCoopVisibility()) { NetLog("[NET/TEST] Enemy visibility probe FAILED"); mSession.Stop(); return; }
                NetLog("[NET/TEST] Enemy occlusion and switching between both players verified");
                mTestEnemyInitialPosition = enemy->GetCharacterBody()->GetPosition(); mTestEnemySpawned = true;
                NetLog("[NET/TEST] Spawned authoritative enemy fixture");
            } else { NetLog("[NET/TEST] Enemy fixture creation FAILED"); mSession.Stop(); return; }
        }
        iLuxEntity* entity = map->GetEntityByName("online_test_grunt");
        if(entity && entity->GetEntityType() == eLuxEntityType_Enemy) {
            iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
            if(!mTestEnemyHunting && now - mSmokeStart > std::chrono::seconds(30)) {
                enemy->SetDisabled(false); enemy->ShowPlayerPosition(); enemy->ChangeState(eLuxEnemyState_Hunt); mTestEnemyHunting = true;
            }
            cVector3f delta = enemy->GetCharacterBody()->GetPosition() - mTestEnemyInitialPosition;
            if(!mTestEnemyMoved && delta.x*delta.x + delta.z*delta.z > 0.01f) {
                mTestEnemyMoved = true; NetLog("[NET/TEST] Host enemy moved under authoritative AI");
            }
            if(!mTestEnemyDamage && (gpBase->mpPlayer->GetHealth() < 99 || gpBase->mpPlayer2->GetHealth() < 99)) {
                mTestEnemyDamage = true; NetLog("[NET/TEST] Host enemy caused player damage");
            }
        }
    }
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_PICKUP") && !mTestPickupSpawned) {
        cLuxPlayer* player = gpBase->mpPlayer2;
        cVector3f position = player->GetCamera()->GetPosition() + player->GetCamera()->GetForward() * 1.2f;
        map->CreateEntity("online_test_tinderbox", "entities/item/tinderbox/tinderbox.ent", cMath::MatrixTranslate(position), cVector3f(1));
        iLuxEntity* fixture = map->GetEntityByName("online_test_tinderbox");
        if(fixture && fixture->GetEntityType() == eLuxEntityType_Prop) {
            iLuxProp* prop = static_cast<iLuxProp*>(fixture);
            for(int i = 0; i < prop->GetBodyNum(); ++i) { prop->GetBody(i)->SetMass(0); }
            mTestInitialTinderboxes = player->GetTinderboxes(); mTestPickupSpawned = true;
            player->SetHealth(50);
            gpBase->mpInventory->AddItem("online_test_potion", eLuxItemType_Health, "Health", "potion_health.tga", 25, "", "");
            gpBase->mpInventory->AddItem("online_test_potion", eLuxItemType_Health, "Health", "potion_health.tga", 25, "", "");
            NetLog("[NET/TEST] Spawned remote pickup fixture");
            gpBase->mpMessageHandler->SetMessage(L"Online story relay test", 0.5f, 0);
        }
    }
    if(mTestPickupSpawned && !mTestPickupFinished && !map->GetEntityByName("online_test_tinderbox")) {
        mTestPickupFinished = true;
        NetLog(gpBase->mpPlayer2->GetTinderboxes() == mTestInitialTinderboxes + 1 ?
            "[NET/TEST] Host validated remote pickup; exactly one tinderbox awarded" : "[NET/TEST] Remote pickup resource count FAILED");
    }
    if(mTestPickupSpawned && !mTestPotionUsed) {
        cLuxInventory_Item* potion = gpBase->mpInventory->GetItem("online_test_potion");
        if(potion && potion->GetCount() == 1 && gpBase->mpPlayer2->GetHealth() >= 74) {
            mTestPotionUsed = true; NetLog("[NET/TEST] Host validated remote inventory use; one potion consumed");
        }
    }
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_CAMPAIGN") && mTestPotionUsed && mTestPickupFinished && mObservedRemoteMovement) {
        float elapsed = std::chrono::duration<float>(now - mSmokeStart).count();
        if(!mTestSaveLoaded && elapsed > 8 &&
            (!TestFlag("AMNESIA_NET_TEST_MEDIA") || mTestLanternLocalLogged)) {
            mTestSaveLoaded = true;
            const int tinderboxes = gpBase->mpPlayer2->GetTinderboxes();
            const bool loaded = gpBase->mpSaveHandler->AutoSave() && gpBase->mpSaveHandler->AutoLoad(false);
            NetLog(loaded && gpBase->mpPlayer2 && gpBase->mpPlayer2->GetTinderboxes() == tinderboxes ?
                "[NET/TEST] Host save/load retained remote player state" : "[NET/TEST] Host save/load FAILED");
            return; // old world/players may have been destroyed
        }
        if(mTestSaveLoaded && !mTestTransitionStarted && mBaselineSent && elapsed > 18) {
            mTestTransitionStarted = true;
            gpBase->mpMapHandler->ChangeMap("01_old_archives.map", "PlayerStartArea_1", "", "");
            NetLog("[NET/TEST] Requested authoritative campaign level transition");
        }
        if(mTestTransitionStarted && mEpoch >= 3 && mBaselineSent && !mTestDeathStarted && elapsed > 25) {
            mTestDeathStarted = true;
            gpBase->mpPlayer2->SetHealth(0);
            NetLog("[NET/TEST] Triggered authoritative shared death");
            mTestDeathObserved = gpBase->mpPlayer->IsDead() && gpBase->mpPlayer2->IsDead();
            NetLog("[NET/TEST] Death health host=" + std::to_string(gpBase->mpPlayer->GetHealth()) + " remote=" + std::to_string(gpBase->mpPlayer2->GetHealth()));
        }
        if(mTestDeathObserved && !mTestRevived && !gpBase->mpPlayer->IsDead() && !gpBase->mpPlayer2->IsDead()) {
            mTestRevived = true;
            NetLog("[NET/TEST] Both players revived at host checkpoint");
        }
    }
    mSnapshotAccumulator += timeStep; mSnapshotBackupAccumulator += timeStep; mWorldReplicationAccumulator += timeStep;
    online::Snapshot snapshot; snapshot.epoch = mEpoch; snapshot.sequence = ++mSnapshotSequence;
    snapshot.acknowledgedInput = mHasLookSequence ? mAppliedLookSequence : 0;
    cLuxPlayer* players[] = {gpBase->mpPlayer, gpBase->mpPlayer2};
    for(int i = 0; i < 2; ++i) {
        auto& p = snapshot.players[i]; cLuxPlayer* player = players[i];
        cVector3f feet = player->GetCharacterBody()->GetFeetPosition();
        p.x = int32_t(feet.x * 1000); p.y = int32_t(feet.y * 1000); p.z = int32_t(feet.z * 1000);
        float yaw = player->GetCamera()->GetYaw(); p.yaw = int16_t(std::atan2(std::sin(yaw), std::cos(yaw)) * 1000);
        p.pitch = int16_t(std::max(-1.571f, std::min(1.571f, player->GetCamera()->GetPitch())) * 1000);
        p.health = uint16_t(std::max(0.0f, std::min(100.0f, player->GetHealth())) * 100);
        p.sanity = uint16_t(std::max(0.0f, std::min(100.0f, player->GetSanity())) * 100);
        p.oil = uint16_t(std::max(0.0f, std::min(100.0f, player->GetLampOil())) * 100);
        p.eyeHeight = int16_t(std::max(-5.0f, std::min(5.0f, player->GetCamera()->GetPosition().y - feet.y)) * 1000);
        cCamera* camera = player->GetCamera();
        const float roll = camera->GetRoll();
        p.roll = int16_t(std::atan2(std::sin(roll), std::cos(roll)) * 1000);
        p.cameraX = int16_t(std::max(-5.0f, std::min(5.0f, camera->GetPosition().x - feet.x)) * 1000);
        p.cameraZ = int16_t(std::max(-5.0f, std::min(5.0f, camera->GetPosition().z - feet.z)) * 1000);
        p.fov = uint16_t(std::max(0.01f, std::min(3.142f, camera->GetFOV())) * 1000);
        const cVector2f screen = gpBase->mpEngine->GetGraphics()->GetLowLevel()->GetScreenSizeFloat();
        p.aspect = uint16_t(std::max(0.01f, std::min(20.0f, camera->GetAspect() / (screen.x / screen.y))) * 1000);
        p.lookSpeed = uint16_t(std::max(0.0f, std::min(10.0f, player->GetLookSpeedMul())) * 1000);
        p.tinderboxes = uint16_t(std::max(0, std::min(65535, player->GetTinderboxes())));
        auto* body = player->GetCharacterBody();
        auto fixedSpeed = [](float value) { return uint16_t(std::max(0.0f,std::min(65.0f,value))*1000); };
        p.forwardSpeed = int16_t(std::max(-32.0f,std::min(32.0f,body->GetMoveSpeed(eCharDir_Forward)))*1000);
        p.sideSpeed = int16_t(std::max(-32.0f,std::min(32.0f,body->GetMoveSpeed(eCharDir_Right)))*1000);
        p.forwardMax = fixedSpeed(body->GetMaxPositiveMoveSpeed(eCharDir_Forward)); p.sideMax = fixedSpeed(body->GetMaxPositiveMoveSpeed(eCharDir_Right));
        p.backwardMax = fixedSpeed(std::fabs(body->GetMaxNegativeMoveSpeed(eCharDir_Forward)));
        p.acceleration = fixedSpeed(body->GetMoveAcc(eCharDir_Forward)); p.deceleration = fixedSpeed(body->GetMoveDeacc(eCharDir_Forward));
        auto signedSpeed=[](float value){return int16_t(std::max(-32.0f,std::min(32.0f,value))*1000);};
        p.verticalSpeed=signedSpeed(body->GetForceVelocity().y);
        cVector3f gravity=body->GetCustomGravityActive()?body->GetCustomGravity():map->GetPhysicsWorld()->GetGravity();
        auto* move=static_cast<cLuxMoveState_Normal*>(player->GetMoveStateData(eLuxMoveState_Normal));
        p.verticalAcceleration=signedSpeed(body->GravityIsActive()?gravity.y*move->GetJumpGravityMultiplier():0);
        p.grounded=body->IsOnGround()?1:0;
        if(player->GetCurrentMoveState()==eLuxMoveState_Normal && player->GetCurrentState()==eLuxPlayerState_Normal &&
            !player->GetJumpDisabled() && !player->GetInsanityCollapse()->IsActive())
            p.jumpSpeed=fixedSpeed(move->GetJumpStartForce()*player->GetScriptJumpForceMul()*
                player->GetDefaultMass()/std::max(.001f,body->GetMass())*gpBase->mpEngine->GetStepSize());
        p.flags = (player->GetCharacterBody()->GetActiveSize() == 1 ? 1 : 0) |
            (player->GetHelperLantern()->IsActive() ? 2 : 0) | (player->IsActive() ? 4 : 0) | (player->IsDead() ? 8 : 0) |
            (player->GetHelperLookAt()->IsActive() || player->GetLookSpeedMul() <= 0 ? 16 : 0) |
            (player->NetworkObjectControlsLook() ? 32 : 0);
    }
    if(map->GetName() == "00_rainy_hall") {
        const auto* intro = map->GetVar("IntroSequenceState");
        const auto* wake = map->GetVar("BlackoutStep");
        const bool sharedIntro = intro->msVal.empty() ? !gpBase->mpPlayer->IsActive() : cString::ToInt(wake->msVal.c_str(),0) < 18;
        if(sharedIntro) snapshot.players[1].flags |= 64;
    }
    if(!mHasHostLife || ((snapshot.players[0].flags ^ mHostLife.players[0].flags) & 12) ||
        ((snapshot.players[1].flags ^ mHostLife.players[1].flags) & 12)) {
        if(!mSession.SendLife(snapshot)) { NetLog("[NET/STATE] Cannot send life transition; closing session"); mSession.Stop(); return; }
    }
    // Keep fallback intervals short without increasing world/media scan frequency.
    if(!mHasHostLife || mSnapshotBackupAccumulator >= 0.1f ||
        ((snapshot.players[0].flags ^ mHostLife.players[0].flags) & 2) ||
        ((snapshot.players[1].flags ^ mHostLife.players[1].flags) & 2)) {
        if(!mSession.SendSnapshotBackup(snapshot)) { NetLog("[NET/STATE] Cannot send pose backup; closing session"); mSession.Stop(); return; }
        mSnapshotBackupAccumulator = 0;
    }
    mHostLife = snapshot; mHasHostLife = true;
    if(mSnapshotAccumulator >= 1.0f / 60.0f) {
        mSnapshotAccumulator = std::fmod(mSnapshotAccumulator, 1.0f / 60.0f);
        // QA fault injection suppresses only ordinary poses, retaining reliable data.
        const float testElapsed = mSmokeSeconds ? std::chrono::duration<float>(now - mSmokeStart).count() : 0;
        const bool blackout = mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_SNAPSHOT_BLACKOUT") && testElapsed >= 5 && testElapsed < 8;
        if(blackout && !mTestSnapshotBackupLogged) {
            mTestSnapshotBackupLogged = true; NetLog("[NET/TEST] Ordinary snapshots suppressed for three seconds");
        }
        const bool snapshotSent = !blackout && mSession.SendSnapshot(snapshot);
        const bool dead = (snapshot.players[1].flags & 8) != 0;
        if(mSmokeSeconds && snapshotSent && (!mTestHasLifeSnapshot || mTestLastSentDead != dead)) {
            mTestHasLifeSnapshot = true; mTestLastSentDead = dead;
            NetLog("[NET/TEST] Sent remote life snapshot dead=" + std::to_string(dead ? 1 : 0) + " sequence=" + std::to_string(snapshot.sequence));
        }
    }
    if(mWorldReplicationAccumulator < 1.0f / 20.0f) return;
    mWorldReplicationAccumulator = std::fmod(mWorldReplicationAccumulator, 1.0f / 20.0f);
    const bool mediaReady = ReplicateMedia(map);
    if(!mSession.Connected()) return;
    if(mediaReady) ReplicateWorld(map);
    ReplicateLights(map);
    for(unsigned count = 0; count < 64 && !mPendingPresentation.empty(); ++count) {
        auto event = mPendingPresentation.front(); event.epoch = mEpoch;
        if(!mSession.SendPresentation(event)) break;
        mPendingPresentation.pop_front();
    }
    online::InventoryState inventory = gpBase->mpInventory->CaptureNetworkInventory(mEpoch);
    auto journal = gpBase->mpJournal->CaptureNetworkJournal(mEpoch);
    auto journalBytes = online::EncodeJournal(journal);
    const bool journalUnsendable = journalBytes.empty();
    if(!journalBytes.empty() && journalBytes != mJournalBytes) {
        journal.revision = ++mJournalRevision;
        if(mSession.SendJournal(journal)) mJournalBytes = std::move(journalBytes);
    }
    auto inventoryBytes = online::EncodeInventory(inventory);
    const bool inventoryUnsendable = inventoryBytes.empty();
    if(!inventoryBytes.empty() && inventoryBytes != mInventoryBytes) {
        inventory.revision = ++mInventoryRevision;
        if(mSession.SendInventory(inventory)) mInventoryBytes = std::move(inventoryBytes);
    }
    // The client waits for both before confirming the world, so an unsendable one
    // froze the host in the loading barrier without a word (2026-11-10).
    if(journalUnsendable && mUnsendableLoggedEpoch[0] != mEpoch) {
        mUnsendableLoggedEpoch[0] = mEpoch;
        NetLog("[NET/JOURNAL] Cannot encode journal entries=" + std::to_string(journal.entries.size()) + "; the client cannot finish loading");
    }
    if(inventoryUnsendable && mUnsendableLoggedEpoch[1] != mEpoch) {
        mUnsendableLoggedEpoch[1] = mEpoch;
        std::string invalid;
        for(const auto& item : inventory.items) {
            online::InventoryState single; single.epoch = mEpoch; single.items.push_back(item);
            if(online::EncodeInventory(single).empty()) { invalid = item.name; break; }
        }
        NetLog("[NET/INVENTORY] Cannot encode inventory items=" + std::to_string(inventory.items.size()) +
            " first_invalid=" + invalid + "; the client cannot finish loading");
    }
}
void cLuxNetwork::ReplicateWorld(cLuxMap* map) {
    std::map<uint32_t, bool> present;
    std::vector<PendingEntity> pending;
    unsigned sent = 0;
    cLuxEntityIterator it = map->GetEntityIterator();
    while(it.HasNext()) {
        iLuxEntity* entity = it.Next();
        if(entity->GetEntityType() > eLuxEntityType_Enemy) continue;
        online::EntityState state; state.epoch = mEpoch; state.id = uint32_t(entity->GetID());
        state.name = entity->GetName(); state.type = uint8_t(entity->GetEntityType());
        state.flags = (entity->IsActive() ? 1 : 0) | (entity->GetInteractionDisabled() ? 2 : 0);
        present[state.id] = true;
        cVector3f scale(1);
        cMeshEntity* mesh = entity->GetMeshEntity();
        state.transform = PackTransform(mesh ? mesh->GetWorldMatrix() : cMatrixf::Identity);
        if(state.type == eLuxEntityType_Prop) {
            iLuxProp* prop = static_cast<iLuxProp*>(entity); state.subtype = uint16_t(prop->GetPropType());
            state.file = prop->GetNetworkFile(); scale = prop->GetNetworkScale();
            if(prop->GetEffectsActive()) state.flags |= 64;
            if(prop->GetPropType() == eLuxPropType_Lamp && static_cast<cLuxProp_Lamp*>(prop)->GetLit()) state.flags |= 8;
            if(prop->GetPropType() == eLuxPropType_SwingDoor) {
                auto* door = static_cast<cLuxProp_SwingDoor*>(prop);
                if(door->GetLocked()) state.flags |= 16; if(door->GetClosed()) state.flags |= 32;
                // Broken by an enemy, a script or a thrown object: the leaf is gone on the host.
                if(door->IsBroken()) state.flags |= 128;
            }
            for(int i = 0; i < prop->GetBodyNum() && i < 32; ++i) state.bodies.push_back(PackTransform(prop->GetBody(i)->GetLocalMatrix()));
        } else if(state.type == eLuxEntityType_Enemy) {
            iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
            state.file = enemy->GetNetworkFile(); scale = enemy->GetNetworkScale();
            if(enemy->GetCharacterBody()) state.bodies.push_back(PackTransform(enemy->GetBody(0)->GetLocalMatrix()));
        }
        std::replace(state.file.begin(), state.file.end(), '\\', '/');
        // Scripts may root asset names, e.g. 07_archives_cellar creates its bait
        // from "/organic/...". The engine resolves both forms by name; the wire
        // format keeps rejecting rooted paths.
        state.file.erase(0, state.file.find_first_not_of('/'));
        // 15 original .ent files name their debris by authoring path, e.g. castle.ent's
        // "F:/Amnesia/redist/entities/door/castle/castle_broken.ent": breaking one closed
        // the session. Resources resolve by name, so send the bare file name.
        if(state.file.find(':') != std::string::npos) state.file = cString::GetFileName(state.file);
        state.scale = {{int32_t(scale.x * 1000), int32_t(scale.y * 1000), int32_t(scale.z * 1000)}};
        if(mesh) for(int i = 0; i < mesh->GetAnimationStateNum() && state.animations.size() < 32; ++i) {
            cAnimationState* animation = mesh->GetAnimationState(i);
            if(!animation->IsActive() || animation->GetWeight() <= 0.001f) continue;
            online::Animation a; a.index = uint16_t(i); a.weight = uint16_t(std::max(0.0f, std::min(1.0f, animation->GetWeight())) * 10000);
            a.time = uint32_t(std::max(0.0f, std::min(86400.0f, animation->GetTimePosition())) * 1000); state.animations.push_back(a);
        }
        auto bytes = online::EncodeEntity(state);
        if(bytes.empty()) {
            NetLog("[NET/WORLD] Unsupported entity state: " + state.name + "; closing session instead of accepting an incomplete baseline");
            mSession.Stop(); return;
        }
        auto old = mEntityBytes.find(state.id);
        const auto previous = mEntityStates.find(state.id);
        const bool changed = old == mEntityBytes.end() || old->second != bytes;
        const bool propertiesChanged = previous == mEntityStates.end() ||
            previous->second.type != state.type || previous->second.flags != state.flags ||
            previous->second.subtype != state.subtype || previous->second.name != state.name ||
            previous->second.file != state.file || previous->second.scale != state.scale ||
            previous->second.bodies.size() != state.bodies.size();
        const auto now = std::chrono::steady_clock::now();
        // Props and enemies move every tick: stream their poses as replaceable
        // unreliable motion and confirm the resting pose reliably once settled.
        // Enemies used to send every pose reliably, in the bulk window shared
        // with checkpoint fragments, so a checkpoint froze the monster on the
        // client until the backlog drained and it "teleported" (2026-10-22).
        const bool liveMover = (state.type == eLuxEntityType_Prop || state.type == eLuxEntityType_Enemy) &&
            mSession.IsWorldApplied(mEpoch);
        const bool settling = liveMover && !changed && !mEntityMotionSettled[state.id] &&
            now - mEntityMotionAt[state.id] >= std::chrono::milliseconds(250);
        const bool repeat = liveMover && !changed && !mEntityMotionSettled[state.id] && !settling;
        if(!changed && !settling && !repeat) continue;
        pending.push_back({std::move(state), std::move(bytes), changed, settling, propertiesChanged, liveMover});
    }
    // At most 64 entity messages per tick. The scan order is fixed, so sending
    // in scan order let the first 64 constantly moving entities (chains, swinging
    // bodies, animated enemies) take every slot and starve the rest -- a door
    // late in the list stayed closed on the client. Serve the longest-unsent first.
    std::stable_sort(pending.begin(), pending.end(), [&](const PendingEntity& a, const PendingEntity& b) {
        const auto ia = mEntitySentAt.find(a.state.id), ib = mEntitySentAt.find(b.state.id);
        const auto ta = ia == mEntitySentAt.end() ? std::chrono::steady_clock::time_point() : ia->second;
        const auto tb = ib == mEntitySentAt.end() ? std::chrono::steady_clock::time_point() : ib->second;
        return ta < tb;
    });
    for(auto& candidate : pending) {
        if(sent >= 64) break;
        auto& state = candidate.state;
        const auto now = std::chrono::steady_clock::now();
        state.sequence = ++mEntitySequence; if(!state.sequence) state.sequence = ++mEntitySequence;
        const bool reliable = !candidate.liveMover || candidate.propertiesChanged || candidate.settling;
        if(!(reliable ? mSession.SendEntity(state) : mSession.SendEntityMotion(state))) continue;
        mEntitySentAt[state.id] = now;
        mEntityBytes[state.id] = std::move(candidate.bytes); mEntityStates[state.id] = state; ++sent;
        if(candidate.changed) mEntityMotionAt[state.id] = now;
        mEntityMotionSettled[state.id] = candidate.settling || (reliable && candidate.propertiesChanged);
    }
    for(auto it = mEntityStates.begin(); it != mEntityStates.end();) {
        if(present.find(it->first) != present.end() || sent >= 64) { ++it; continue; }
        online::EntityState removed = it->second; removed.flags |= 4; removed.bodies.clear(); removed.animations.clear();
        removed.sequence = ++mEntitySequence; if(!removed.sequence) removed.sequence = ++mEntitySequence;
        if(!mSession.SendEntity(removed)) { ++it; continue; }
        mEntityMotionAt.erase(removed.id); mEntityMotionSettled.erase(removed.id); mEntitySentAt.erase(removed.id);
        ++sent; mEntityBytes.erase(it->first); it = mEntityStates.erase(it);
    }
    if(!mBaselineSent) {
        bool complete = true;
        for(const auto& pair : present) if(mEntityStates.find(pair.first) == mEntityStates.end()) { complete = false; break; }
        if(complete && mSession.SendWorldReady(mEpoch)) { mBaselineSent = true; NetLog("[NET/WORLD] Complete host entity baseline sent"); }
    }
}
void cLuxNetwork::ApplyWorldEntity(cLuxMap* map, const online::EntityState& state) {
    iLuxEntity* entity = NULL;
    auto mapped = mReplicaIDs.find(state.id);
    if(mapped != mReplicaIDs.end()) entity = map->GetEntityByID(mapped->second);
    if(!entity) entity = map->GetEntityByID(int(state.id));
    if(!entity || entity->GetName() != state.name || uint8_t(entity->GetEntityType()) != state.type)
        entity = map->GetEntityByName(state.name, eLuxEntityType(state.type));
    if(state.flags & 4) {
        mPropMotion.erase(state.id); mEnemyMotion.erase(state.id);
        if(entity) map->DestroyEntity(entity); mReplicaIDs.erase(state.id);
        if(mSmokeSeconds && state.name == "online_test_tinderbox") NetLog("[NET/TEST] Host pickup destruction replicated to client");
        return;
    }
    if(entity && entity->GetDestroyMe()) entity = NULL;
    if(!entity && !state.file.empty()) {
        cVector3f scale(state.scale[0] / 1000.0f, state.scale[1] / 1000.0f, state.scale[2] / 1000.0f);
        map->CreateEntity(state.name, state.file, UnpackTransform(state.transform), scale);
        entity = map->GetLatestEntity();
    }
    if(!entity || entity->GetName() != state.name || uint8_t(entity->GetEntityType()) != state.type) {
        NetLog("[NET/WORLD] Cannot apply authoritative entity: " + state.name + "; closing incomplete replica");
        mSession.Stop(); return;
    }
    const bool knownProp = mReplicaIDs.count(state.id) && state.type == eLuxEntityType_Prop && mBaselineApplied;
    bool interpolatedEnemy = false;
    const bool knownEnemyReplica = mReplicaIDs.count(state.id) && state.type == eLuxEntityType_Enemy && mBaselineApplied;
    mReplicaIDs[state.id] = entity->GetID();
    entity->SetActive((state.flags & 1) != 0); entity->SetInteractionDisabled((state.flags & 2) != 0);
    if(state.type == eLuxEntityType_Prop) {
        iLuxProp* prop = static_cast<iLuxProp*>(entity);
        if(uint16_t(prop->GetPropType()) != state.subtype) return;
        prop->UseReplicatedMedia();
        prop->SetEffectsActive((state.flags & 64) != 0, false);
        if(prop->GetPropType() == eLuxPropType_Lamp) static_cast<cLuxProp_Lamp*>(prop)->SetLit((state.flags & 8) != 0, false);
        if(prop->GetPropType() == eLuxPropType_SwingDoor) {
            auto* door = static_cast<cLuxProp_SwingDoor*>(prop);
            door->SetLocked((state.flags & 16) != 0, false); door->SetClosed((state.flags & 32) != 0, false);
            door->SetNetworkBroken((state.flags & 128) != 0);
        }
        PropMotion motion; motion.target = state; motion.fromMesh = prop->GetMeshEntity() ? prop->GetMeshEntity()->GetWorldMatrix() : cMatrixf::Identity;
        bool interpolate = knownProp && (state.flags & 1);
        for(size_t i = 0; i < state.bodies.size() && i < size_t(prop->GetBodyNum()); ++i) {
            const cMatrixf from = prop->GetBody(int(i))->GetLocalMatrix();
            const cMatrixf to = UnpackTransform(state.bodies[i]);
            cVector3f delta = to.GetTranslation() - from.GetTranslation();
            if(delta.Length() > 5) interpolate = false; // teleports never slide across walls
            motion.from.push_back(from);
        }
        if(interpolate && !motion.from.empty()) mPropMotion[state.id] = std::move(motion);
        else {
            mPropMotion.erase(state.id);
            for(size_t i = 0; i < state.bodies.size() && i < size_t(prop->GetBodyNum()); ++i)
                prop->GetBody(int(i))->SetMatrix(UnpackTransform(state.bodies[i]));
        }
    } else if(state.type == eLuxEntityType_Enemy && !state.bodies.empty()) {
        iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
        iCharacterBody* enemyBody = enemy->GetCharacterBody();
        cMeshEntity* enemyMesh = enemy->GetMeshEntity();
        const cVector3f to = UnpackTransform(state.bodies[0]).GetTranslation();
        // Nemico già noto e attivo: dalla posa mostrata a quella nuova in 50 ms
        // (l'intervallo di invio). Comparsa, baseline e salti di oltre 5 m
        // vengono applicati subito.
        const bool knownEnemy = knownEnemyReplica && (state.flags & 1);
        if(knownEnemy && enemyBody && enemyMesh && (to - enemyBody->GetPosition()).Length() <= 5) {
            EnemyMotion motion;
            motion.fromPosition = enemyBody->GetPosition(); motion.toPosition = to;
            motion.fromMesh = enemyMesh->GetWorldMatrix(); motion.toMesh = UnpackTransform(state.transform);
            mEnemyMotion[state.id] = motion; interpolatedEnemy = true;
        } else {
            mEnemyMotion.erase(state.id);
            if(enemyBody) enemyBody->SetPosition(to);
        }
        if(mSmokeSeconds && state.name == "online_test_grunt") {
            cVector3f position = UnpackTransform(state.bodies[0]).GetTranslation();
            if(!mTestHasEnemyReplica) { mTestHasEnemyReplica = true; mTestReplicaEnemyInitialPosition = position; NetLog("[NET/TEST] Authoritative enemy created on client"); }
            cVector3f delta = position - mTestReplicaEnemyInitialPosition;
            if(!mTestReplicaEnemyMoved && delta.x*delta.x + delta.z*delta.z > 0.01f) { mTestReplicaEnemyMoved = true; NetLog("[NET/TEST] Authoritative enemy movement applied on client"); }
        }
    }
    cMeshEntity* mesh = entity->GetMeshEntity();
    if(mesh) {
        if((state.type == eLuxEntityType_Enemy && !interpolatedEnemy) || state.bodies.empty()) mesh->SetMatrix(UnpackTransform(state.transform));
        for(int i = 0; i < mesh->GetAnimationStateNum(); ++i) mesh->GetAnimationState(i)->SetActive(false);
        for(const auto& a : state.animations) if(a.index < mesh->GetAnimationStateNum()) {
            cAnimationState* animation = mesh->GetAnimationState(a.index);
            animation->SetActive(true); animation->SetWeight(a.weight / 10000.0f); animation->SetTimePosition(a.time / 1000.0f);
        }
    }
    if(!mLoggedWorldReplica) { mLoggedWorldReplica = true; NetLog("[NET/WORLD] Authoritative entity state applied"); }
}
void cLuxNetwork::UpdateVisibilityPropsTest(cLuxMap* map) {
    if(!map || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer->GetCharacterBody()) return;
    if(!mTestStartingOilObserved) {
        const float oil = gpBase->mpPlayer->GetLampOil();
        const bool full = oil > 99.99f && (IsClientReplica() || (gpBase->mpPlayer2 && gpBase->mpPlayer2->GetLampOil() > 99.99f));
        NetLog(full ? "[NET/TEST] Full starting oil verified on local and remote player" : "[NET/TEST] Starting oil FAILED");
        mTestStartingOilObserved = true;
    }
    if(!IsClientReplica() && !mTestDarknessStarted) {
        auto lights = map->GetWorld()->GetLightIterator();
        while(lights.HasNext()) {
            auto* light = lights.Next();
            if(light->GetName().find("Player") != 0) light->SetDiffuseColor(cColor(0,0));
        }
        gpBase->mpPlayer->GetHelperLantern()->SetActive(false,false,false,false);
        if(gpBase->mpPlayer2) gpBase->mpPlayer2->GetHelperLantern()->SetActive(false,false,false,false);
        mTestDarknessStarted = true;
        NetLog("[NET/TEST] Unlit map fixture enabled without changing installed assets");
    }
    auto* ambient = gpBase->mpPlayer->GetHelperInDarkness()->GetAmbientLight();
    if(!mTestDarknessDiagnostic && std::chrono::steady_clock::now()-mSmokeStart > std::chrono::seconds(20)) {
        mTestDarknessDiagnostic = true;
        NetLog("[NET/QA] Darkness local index=" + std::to_string(gpBase->mpPlayer->GetPlayerIndex()) +
            " level=" + std::to_string(gpBase->mpPlayer->GetHelperLightLevel()->GetNormalLightLevel()) +
            " ambient_r=" + std::to_string(ambient ? ambient->GetDiffuseColor().r : -1) +
            " active=" + std::to_string(ambient && ambient->IsActive()) + " lantern=" + std::to_string(gpBase->mpPlayer->GetHelperLantern()->IsActive()) +
            " container=" + gpBase->mpEngine->GetUpdater()->GetCurrentContainerName());
    }
    if(!mTestDarknessObserved && ambient && !gpBase->mpPlayer->GetHelperLantern()->IsActive() &&
        gpBase->mpPlayer->GetHelperLightLevel()->GetNormalLightLevel() <= gpBase->mpGameCfg->GetFloat("Player_Darkness","AmbientLightMinLightLevel",0) && ambient->GetDiffuseColor().r > 0.001f) {
        mTestDarknessObserved = true;
        NetLog("[NET/TEST] Original eye adaptation emits local ambient light in darkness oil=" + std::to_string(gpBase->mpPlayer->GetLampOil()));
    }
    if(IsClientReplica()) return;
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now()-mSmokeStart).count();
    if(!mTestPropsStarted) {
        const cVector3f origin = gpBase->mpPlayer->GetCharacterBody()->GetFeetPosition() + cVector3f(3,1,0);
        const char* names[] = {"online_test_motion_box","online_test_motion_door"};
        const char* files[] = {"entities/container/wood_box01/wood_box01.ent","entities/door/mansion/mansion.ent"};
        for(int k=0;k<2;++k) {
            map->CreateEntity(names[k],files[k],cMath::MatrixTranslate(origin+cVector3f(float(k*2),0,0)),cVector3f(1));
            auto* entity = map->GetEntityByName(names[k]);
            if(!entity || entity->GetEntityType() != eLuxEntityType_Prop) { NetLog("[NET/TEST] Motion fixture FAILED"); return; }
            auto* prop = static_cast<iLuxProp*>(entity);
            if(k == 1) static_cast<cLuxProp_SwingDoor*>(prop)->SetClosed(false,false);
            for(int i=0;i<prop->GetBodyNum();++i) { prop->GetBody(i)->SetMass(0); mTestPropBase[names[k]].push_back(prop->GetBody(i)->GetLocalMatrix()); }
        }
        mTestPropsStarted = true; NetLog("[NET/TEST] Authoritative box and door motion fixtures created");
    }
    for(const auto& fixture : mTestPropBase) {
        auto* entity = map->GetEntityByName(fixture.first);
        if(!entity || entity->GetEntityType() != eLuxEntityType_Prop) continue;
        auto* prop = static_cast<iLuxProp*>(entity);
        const float t = std::min(40.0f,elapsed);
        for(size_t i=0;i<fixture.second.size();++i) {
            cMatrixf matrix = fixture.second[i];
            if(fixture.first == "online_test_motion_door") {
                cVector3f position = matrix.GetTranslation(); matrix.SetTranslation(cVector3f(0));
                matrix = cMath::MatrixMul(cMath::MatrixRotateY(std::sin(t)*0.5f),matrix); matrix.SetTranslation(position);
            } else matrix.SetTranslation(matrix.GetTranslation()+cVector3f(std::sin(t)*0.5f,0,0));
            prop->GetBody(int(i))->SetMatrix(matrix);
        }
    }
}
void cLuxNetwork::UpdateEnemyMotion(cLuxMap* map, float timeStep) {
    for(auto it = mEnemyMotion.begin(); it != mEnemyMotion.end();) {
        auto replica = mReplicaIDs.find(it->first);
        iLuxEntity* entity = replica == mReplicaIDs.end() ? NULL : map->GetEntityByID(replica->second);
        if(!entity || entity->GetDestroyMe() || entity->GetEntityType() != eLuxEntityType_Enemy) { it = mEnemyMotion.erase(it); continue; }
        auto* enemy = static_cast<iLuxEnemy*>(entity); auto& motion = it->second;
        motion.elapsed += std::max(0.0f,timeStep);
        const float alpha = std::min(1.0f,motion.elapsed / 0.05f);
        if(auto* body = enemy->GetCharacterBody()) body->SetPosition(motion.fromPosition + (motion.toPosition - motion.fromPosition) * alpha);
        if(auto* mesh = enemy->GetMeshEntity()) mesh->SetMatrix(cMath::MatrixSlerp(alpha,motion.fromMesh,motion.toMesh,true));
        if(mSmokeSeconds && !mTestEnemyInterpolated && alpha > 0 && alpha < 1 && entity->GetName() == "online_test_grunt" &&
           (motion.toPosition - motion.fromPosition).Length() > 0.001f) {
            mTestEnemyInterpolated = true; NetLog("[NET/TEST] Enemy rendered between authoritative poses");
        }
        if(alpha >= 1) it = mEnemyMotion.erase(it); else ++it;
    }
}
void cLuxNetwork::UpdatePropMotion(cLuxMap* map, float timeStep) {
    for(auto it = mPropMotion.begin(); it != mPropMotion.end();) {
        auto replica = mReplicaIDs.find(it->first);
        iLuxEntity* entity = replica == mReplicaIDs.end() ? NULL : map->GetEntityByID(replica->second);
        if(!entity || entity->GetDestroyMe() || entity->GetEntityType() != eLuxEntityType_Prop) { it = mPropMotion.erase(it); continue; }
        auto* prop = static_cast<iLuxProp*>(entity); auto& motion = it->second;
        motion.elapsed += std::max(0.0f,timeStep);
        const float alpha = std::min(1.0f,motion.elapsed / 0.05f);
        for(size_t i = 0; i < motion.from.size() && i < size_t(prop->GetBodyNum()); ++i)
            prop->GetBody(int(i))->SetMatrix(cMath::MatrixSlerp(alpha,motion.from[i],UnpackTransform(motion.target.bodies[i]),true));
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_VISIBILITY_PROPS") && motion.target.name.find("online_test_motion_") == 0) {
            const unsigned bit = motion.target.name == "online_test_motion_box" ? 1 : 2;
            const auto from = PackTransform(motion.from[0]);
            const auto shown = PackTransform(prop->GetBody(0)->GetLocalMatrix());
            if(alpha > 0 && alpha < 1 && !(mTestPropInterpolated & bit) && shown != from && shown != motion.target.bodies[0]) {
                mTestPropInterpolated |= bit;
                NetLog("[NET/TEST] Prop rendered between authoritative poses: " + motion.target.name);
            }
            if(alpha >= 1 && !motion.target.liveMotion && !(mTestPropSettled & bit) && std::chrono::steady_clock::now()-mSmokeStart > std::chrono::seconds(35)) {
                bool equal = true;
                for(size_t i=0;i<motion.from.size();++i) {
                    auto actual = PackTransform(prop->GetBody(int(i))->GetLocalMatrix());
                    for(size_t j=0;j<actual.size();++j) if(std::abs(int64_t(actual[j])-motion.target.bodies[i][j]) > 2) equal = false;
                }
                if(equal) { mTestPropSettled |= bit; NetLog("[NET/TEST] Settled authoritative prop pose verified: " + motion.target.name); }
            }
        }
        if(alpha >= 1) it = mPropMotion.erase(it); else ++it;
    }
}
bool cLuxNetwork::ReplicateMedia(cLuxMap* map) {
    cWorld* world = map->GetWorld();
    EnsureMediaIDs(world, mNextMediaID);
    std::map<MediaKey, online::MediaState> current;
    auto quantize = [](float v) { return int32_t(std::round(std::max(0.0f, std::min(100000.0f, v)) * 1000)); };
    auto sounds = world->GetSoundEntityIterator();
    while(sounds.HasNext()) {
        cSoundEntity* sound = sounds.Next();
        online::MediaState s; s.epoch = mEpoch; s.id = uint32_t(sound->GetUniqueID()) + 1; s.kind = online::MediaKind::Sound;
        s.name = sound->GetName(); s.file = MediaAssetName(sound->GetData()->GetName());
        s.transform = PackTransform(sound->GetWorldMatrix());
        s.flags = (!sound->IsStopped() ? 1 : 0) | (sound->GetForcePlayAsGUISound() ? 2 : 0) |
            (sound->GetRemoveWhenOver() ? 4 : 0) | (sound->IsSaved() ? 8 : 0) | (sound->IsActive() ? 16 : 0) |
            (sound->GetData()->GetLoop() ? 32 : 0) | (sound->IsFadingOut() ? 64 : 0);
        s.values[0] = quantize(sound->GetVolume()); s.values[1] = quantize(sound->GetMinDistance()); s.values[2] = quantize(sound->GetMaxDistance());
        s.values[3] = s.values[4] = s.values[6] = 1000;
        cSoundEntry* entry = sound->GetSoundEntry(eSoundEntityType_Main, true);
        if(entry) {
            s.values[3] = quantize(entry->GetVolumeMul()); s.values[4] = quantize(entry->GetVolumeFadeDest());
            // HPL stores the fade slope with its direction; the wire carries
            // its magnitude because the destination determines the sign.
            s.values[5] = quantize(std::fabs(entry->GetVolumeFadeSpeed())); s.values[6] = quantize(entry->GetSpeedMul());
        }
        current[{uint8_t(s.kind), s.id}] = std::move(s);
    }
    auto particles = world->GetParticleSystemIterator();
    while(particles.HasNext()) {
        cParticleSystem* ps = particles.Next();
        online::MediaState s; s.epoch = mEpoch; s.id = uint32_t(ps->GetUniqueID()) + 1; s.kind = online::MediaKind::Particle;
        s.name = ps->GetName(); s.file = MediaAssetName(ps->GetDataName());
        s.transform = PackTransform(ps->GetWorldMatrix());
        s.flags = (ps->IsActive() ? 1 : 0) | (ps->IsVisible() ? 2 : 0) | (ps->GetFadeAtDistance() ? 4 : 0) |
            (ps->IsSaved() ? 8 : 0) | (ps->IsDying() ? 16 : 0) | (ps->GetRemoveWhenDead() ? 32 : 0);
        const cColor color = ps->GetColor();
        s.values = {{quantize(color.r), quantize(color.g), quantize(color.b), quantize(color.a),
            quantize(ps->GetMinFadeDistanceEnd()), quantize(ps->GetMinFadeDistanceStart()),
            quantize(ps->GetMaxFadeDistanceStart()), quantize(ps->GetMaxFadeDistanceEnd()),
            std::max(1, quantize(ps->GetDataSize().x)), std::max(1, quantize(ps->GetDataSize().y)), std::max(1, quantize(ps->GetDataSize().z)), 0}};
        current[{uint8_t(s.kind), s.id}] = std::move(s);
    }
    for(int priority = 0; priority <= gpBase->mpMusicHandler->GetMaxPrio(); ++priority) {
        cLuxMusic* music = gpBase->mpMusicHandler->GetMusic(priority);
        online::MediaState s; s.epoch = mEpoch; s.id = priority + 1; s.kind = online::MediaKind::Music; s.file = MediaAssetName(music->msFile);
        s.values[1] = quantize(mMusicFadeTimes.count(priority) ? mMusicFadeTimes[priority] : 1.0f);
        std::replace(s.file.begin(), s.file.end(), '\\', '/');
        if(s.file.empty()) s.flags = 128;
        else {
            s.flags = (music->mbLoop ? 1 : 0) | (music->mbResume ? 2 : 0) | (music->mbSpecialEffect ? 4 : 0) |
                (gpBase->mpMusicHandler->GetCurrentMaxPrio() == priority ? 8 : 0);
            const float mul = gpBase->mpMusicHandler->GetVolumeMul();
            s.values[0] = quantize(mul > 0 ? music->mfVolume / mul : 0);
        }
        current[{uint8_t(s.kind), s.id}] = std::move(s);
    }
    if(current.size() > 2048) { NetLog("[NET/MEDIA] Media roster exceeded; closing session"); mSession.Stop(); return false; }
    unsigned sent = 0; bool complete = true;
    for(auto& pair : current) {
        // Playback position is sent only when the music state changes, never
        // used as a delta key that would seek the stream on every logic tick.
        auto bytes = online::EncodeMedia(pair.second);
        if(bytes.empty()) { NetLog("[NET/MEDIA] Unsupported media: " + pair.second.name + " file=" + pair.second.file); mSession.Stop(); return false; }
        auto old = mMediaBytes.find(pair.first);
        if(old != mMediaBytes.end() && old->second == bytes) continue;
        if(sent >= 64) { complete = false; continue; }
        auto wire = pair.second;
        if(wire.kind == online::MediaKind::Music && (wire.flags & 8)) {
            cMusicEntry* song = gpBase->mpEngine->GetSound()->GetMusicHandler()->GetCurrentSong();
            if(song && song->mpStream) wire.values[2] = int32_t(std::max(0.0, std::min(86400.0, song->mpStream->GetElapsedTime())) * 1000);
        }
        if(!mSession.SendMedia(wire)) { complete = false; continue; } // retry after transport backpressure drains
        mMediaBytes[pair.first] = std::move(bytes); mMediaStates[pair.first] = pair.second; ++sent;
    }
    for(auto it = mMediaStates.begin(); it != mMediaStates.end();) {
        if(current.count(it->first)) { ++it; continue; }
        if(sent >= 64) { complete = false; ++it; continue; }
        auto removed = it->second; removed.flags = 128;
        if(!mSession.SendMedia(removed)) { complete = false; ++it; continue; }
        mMediaBytes.erase(it->first); it = mMediaStates.erase(it); ++sent;
    }
    auto* handler = gpBase->mpEngine->GetSound()->GetSoundHandler();
    for(int i = 0; i < 2; ++i) {
        auto* settings = i == 0 ? handler->GetGlobalVolumeSettingsHandler() : handler->GetGlobalSpeedSettingsHandler();
        online::Presentation event; event.epoch = mEpoch;
        event.kind = i == 0 ? online::PresentationKind::GlobalVolume : online::PresentationKind::GlobalSpeed;
        event.values[0] = quantize(settings->GetEntry(eLuxGlobalVolumeType_Script, true)->GetVal());
        auto bytes = online::EncodePresentation(event);
        if(bytes != mGlobalAudioBytes[i] && mSession.SendPresentation(event)) mGlobalAudioBytes[i] = std::move(bytes);
    }
    return complete;
}
void cLuxNetwork::ApplyMedia(cLuxMap* map, const online::MediaState& s) {
    cWorld* world = map->GetWorld(); const MediaKey key(uint8_t(s.kind), s.id);
    auto previous = mMediaStates.find(key);
    if(s.kind == online::MediaKind::Music) {
        const int priority = int(s.id) - 1;
        if(priority > gpBase->mpMusicHandler->GetMaxPrio()) { NetLog("[NET/MEDIA] Unsupported music priority"); mSession.Stop(); return; }
        if(s.flags & 128) gpBase->mpMusicHandler->Stop(s.values[1] / 1000.0f, priority);
        else {
            gpBase->mpMusicHandler->Play(s.file, (s.flags & 1) != 0, s.values[0] / 1000.0f,
                s.values[1] / 1000.0f, priority, (s.flags & 2) != 0, (s.flags & 4) != 0);
            if(s.flags & 8) {
                cMusicEntry* song = gpBase->mpEngine->GetSound()->GetMusicHandler()->GetCurrentSong();
                if(song && song->mpStream && song->msFileName == s.file) {
                    double position = (s.values[2] + mSession.Ping() * 0.5) / 1000.0;
                    const double length = song->mpStream->GetTotalTime();
                    if(length > 0) position = (s.flags & 1) ? std::fmod(position, length) : std::min(position, length);
                    song->mpStream->SetElapsedTime(position);
                }
            }
        }
        mMediaStates[key] = s;
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA") && s.id == 8) NetLog((s.flags & 128) ? "[NET/TEST] Music stop applied on client" : "[NET/TEST] Music state applied on client");
        return;
    }
    auto mapped = mReplicaMediaIDs.find(key);
    cSoundEntity* sound = s.kind == online::MediaKind::Sound && mapped != mReplicaMediaIDs.end() ? world->GetSoundEntityFromUniqueID(mapped->second) : NULL;
    cParticleSystem* ps = s.kind == online::MediaKind::Particle && mapped != mReplicaMediaIDs.end() ? world->GetParticleSystemFromUniqueID(mapped->second) : NULL;
    if(s.flags & 128) {
        if(sound) world->DestroySoundEntity(sound);
        if(ps) world->DestroyParticleSystem(ps);
        mReplicaMediaIDs.erase(key); mMediaStates.erase(key);
        if(mSmokeSeconds && s.name.find("online_media_fixture") == 0) NetLog("[NET/TEST] Media removal applied on client kind=" + std::to_string(uint8_t(s.kind)));
        return;
    }
    // A finite emitter/one-shot may finish locally slightly earlier. Retain
    // its ID until the host removes it instead of replaying it every update.
    if(mapped != mReplicaMediaIDs.end() && !sound && !ps) return;
    if(mapped == mReplicaMediaIDs.end()) {
        auto claimed = [&](int id) { for(const auto& pair : mReplicaMediaIDs) if(pair.first.first == key.first && pair.second == id) return true; return false; };
        if(s.kind == online::MediaKind::Sound) {
            auto it = world->GetSoundEntityIterator();
            while(it.HasNext()) { auto* candidate = it.Next(); if(candidate->GetName() == s.name && MediaAssetName(candidate->GetData()->GetName()) == s.file && !claimed(candidate->GetUniqueID())) { sound = candidate; break; } }
            if(!sound) sound = world->CreateSoundEntity(s.name, s.file, (s.flags & 4) != 0);
            if(sound) { if(sound->GetUniqueID() < 0) sound->SetUniqueID(mNextMediaID++); mReplicaMediaIDs[key] = sound->GetUniqueID(); }
        } else {
            auto it = world->GetParticleSystemIterator();
            while(it.HasNext()) { auto* candidate = it.Next(); if(candidate->GetName() == s.name && MediaAssetName(candidate->GetDataName()) == s.file && !claimed(candidate->GetUniqueID())) { ps = candidate; break; } }
            if(!ps) ps = world->CreateParticleSystem(s.name, s.file, cVector3f(s.values[8] / 1000.0f, s.values[9] / 1000.0f, s.values[10] / 1000.0f), (s.flags & 32) != 0);
            if(ps) { if(ps->GetUniqueID() < 0) ps->SetUniqueID(mNextMediaID++); mReplicaMediaIDs[key] = ps->GetUniqueID(); }
        }
        if(!sound && !ps) { NetLog("[NET/MEDIA] Cannot create media: " + s.name + " file=" + s.file); mSession.Stop(); return; }
        iEntity3D* object = sound ? static_cast<iEntity3D*>(sound) : static_cast<iEntity3D*>(ps);
        if(object->GetEntityParent()) object->GetEntityParent()->RemoveChild(object);
        if(object->GetParent()) object->GetParent()->RemoveEntity(object);
        if(mSmokeSeconds && s.name.find("online_media_fixture") == 0) NetLog("[NET/TEST] Media created on client kind=" + std::to_string(uint8_t(s.kind)));
    }
    if(sound) {
        sound->SetWorldMatrix(UnpackTransform(s.transform)); sound->SetActive((s.flags & 16) != 0);
        sound->SetIsSaved((s.flags & 8) != 0); sound->SetForcePlayAsGUISound((s.flags & 2) != 0);
        sound->SetVolume(s.values[0] / 1000.0f); sound->SetMinDistance(s.values[1] / 1000.0f); sound->SetMaxDistance(s.values[2] / 1000.0f);
        if(!(s.flags & 1)) { if(!sound->IsStopped()) sound->Stop(false); }
        else if(previous != mMediaStates.end() && !(previous->second.flags & 1)) sound->Play(false);
        cSoundEntry* entry = sound->GetSoundEntry(eSoundEntityType_Main, true);
        if(entry) {
            entry->SetVolumeMul(s.values[3] / 1000.0f); entry->SetSpeedMul(s.values[6] / 1000.0f);
            if(s.flags & 64) sound->FadeOut(std::max(0.001f, s.values[5] / 1000.0f));
            else if(s.values[3] != s.values[4] && s.values[5] > 0) entry->FadeVolumeMulTo(s.values[4] / 1000.0f, s.values[5] / 1000.0f);
        }
    }
    if(ps) {
        ps->SetWorldMatrix(UnpackTransform(s.transform)); ps->SetActive((s.flags & 1) != 0); ps->SetVisible((s.flags & 2) != 0);
        ps->SetIsSaved((s.flags & 8) != 0); ps->SetFadeAtDistance((s.flags & 4) != 0); ps->SetRemoveWhenDead((s.flags & 32) != 0);
        ps->SetColor(cColor(s.values[0] / 1000.0f, s.values[1] / 1000.0f, s.values[2] / 1000.0f, s.values[3] / 1000.0f));
        ps->SetMinFadeDistanceEnd(s.values[4] / 1000.0f); ps->SetMinFadeDistanceStart(s.values[5] / 1000.0f);
        ps->SetMaxFadeDistanceStart(s.values[6] / 1000.0f); ps->SetMaxFadeDistanceEnd(s.values[7] / 1000.0f);
        if((s.flags & 16) && !ps->IsDying()) ps->Kill();
    }
    mMediaStates[key] = s;
    if(mSmokeSeconds && s.name.find("online_media_fixture") == 0) {
        if(mEpoch >= 2) NetLog("[NET/TEST] Saved media baseline restored on client kind=" + std::to_string(uint8_t(s.kind)));
        if(s.values[0] == 150) NetLog("[NET/TEST] Media property delta applied on client kind=" + std::to_string(uint8_t(s.kind)));
    }
}
void cLuxNetwork::UpdateMediaTest(cLuxMap* map, float elapsed) {
    if(!mTestLanternDrainRequested && mTestRevived && elapsed > 40) {
        // Revival can reset lantern state. Prepare a silent authoritative
        // fixture, then let the normal oil-drain update turn it off.
        gpBase->mpPlayer2->GetHelperLantern()->SetActive(true, false, false, false);
        gpBase->mpPlayer2->SetLampOil(0.001f);
        mTestLanternDrainRequested = true;
        NetLog("[NET/TEST] Requested authoritative remote lantern oil depletion");
    }
    cWorld* world = map->GetWorld();
    if(!mTestMediaSpawned && mBaselineSent && elapsed > 3) {
        // Reset must clear even a non-finite fade speed before checkpoint capture.
        auto* effects = gpBase->mpEffectHandler;
        effects->GetImageTrail()->FadeTo(0, std::numeric_limits<float>::quiet_NaN());
        effects->GetSepiaColor()->FadeTo(0, std::numeric_limits<float>::quiet_NaN());
        effects->GetRadialBlur()->FadeTo(0, std::numeric_limits<float>::quiet_NaN());
        effects->GetImageTrail()->Reset(); effects->GetImageTrail()->SetActive(false);
        effects->GetSepiaColor()->Reset(); effects->GetSepiaColor()->SetActive(false);
        effects->GetRadialBlur()->Reset(); effects->GetRadialBlur()->SetActive(false);
        cLuxEffectHandler_SaveData savedEffects; savedEffects.FromEffectHandler(effects);
        if(savedEffects.mfImageTrail_FadeSpeed != 0 || savedEffects.mfSepiaColor_FadeSpeed != 0 || savedEffects.mfRadialBlur_FadeSpeed != 0) {
            NetLog("[NET/TEST] Effect reset left invalid checkpoint fade speeds FAILED"); mSession.Stop(); return;
        }
        NetLog("[NET/TEST] Effect reset clears non-finite checkpoint fade speeds");
        auto* expired = world->CreateParticleSystem("online_media_expired_pointer", "ps_dust_falling.ps", cVector3f(1), false);
        if(!expired) { NetLog("[NET/TEST] FAIL: expired particle fixture could not load"); return; }
        world->DestroyParticleSystem(expired);
        world->DestroyParticleSystem(expired); // Deliberately stale owner cache; must not dereference or delete twice.
        NetLog("[NET/TEST] Repeated particle destruction safely ignores stale owner pointer");
        auto* sound = world->CreateSoundEntity("online_media_fixture_sound", "ambience_wind_eerie.snt", false);
        auto* ps = world->CreateParticleSystem("online_media_fixture_particle", "ps_dust_falling.ps", cVector3f(1), false);
        if(!sound || !ps) { NetLog("[NET/TEST] Media fixture creation FAILED"); mSession.Stop(); return; }
        const cVector3f position = gpBase->mpPlayer2->GetCamera()->GetPosition() + cVector3f(0, 1, 0);
        sound->SetPosition(position); sound->SetIsSaved(true); sound->SetVolume(0.35f);
        ps->SetPosition(position); ps->SetIsSaved(true); ps->SetColor(cColor(0.8f, 0.5f, 0.7f, 1));
        gpBase->mpMusicHandler->Play("00_event_hallway.ogg", true, 0.2f, 0.25f, 7, true, false);
        gpBase->mpInventory->AddItem("online_lantern_fixture", eLuxItemType_Lantern, "Lantern", "lantern.tga", 0, "", "");
        gpBase->mpPlayer->SetLampOil(60); gpBase->mpPlayer2->SetLampOil(40);
        gpBase->mpPlayer->GetHelperLantern()->SetActive(true, false);
        mTestMediaSpawned = true; NetLog("[NET/TEST] Saved sound/particle and music fixtures created on host");
    }
    if(mTestMediaSpawned && !mTestMediaRestoreLogged && mEpoch >= 2) {
        if(world->GetSoundEntity("online_media_fixture_sound") && world->GetParticleSystem("online_media_fixture_particle")) {
            mTestMediaRestoreLogged = true; NetLog("[NET/TEST] Host save/load retained sound and particle");
        }
    }
    if(mTestMediaSpawned && !mTestLanternLocalLogged && gpBase->mpPlayer2->GetHelperLantern()->IsActive() &&
        gpBase->mpPlayer->GetHelperLantern()->IsActive() && gpBase->mpPlayer->GetLampOil() - gpBase->mpPlayer2->GetLampOil() > 10) {
        mTestLanternLocalLogged = true; NetLog("[NET/TEST] Host validated remote lantern with independent oil");
    }
    if(mTestMediaSpawned && !mTestMediaChanged && elapsed > 12 && mTestMediaRestoreLogged) {
        auto* sound = world->GetSoundEntity("online_media_fixture_sound"); auto* ps = world->GetParticleSystem("online_media_fixture_particle");
        if(!sound || !ps) { NetLog("[NET/TEST] Saved media missing after load FAILED"); mSession.Stop(); return; }
        sound->SetVolume(0.15f); ps->SetColor(cColor(0.15f, 0.8f, 0.3f, 1)); ps->SetPosition(ps->GetWorldPosition() + cVector3f(0.5f, 0, 0));
        gpBase->mpEffectHandler->GetScreenShake()->Start(0.05f, 0.5f, 0.1f, 0.1f, 1);
        map->RunScript("PlayGuiSound(\"ui_use_health\", 0.2f); FadeGlobalSoundVolume(0.8f, 0.25f); FadeGlobalSoundSpeed(1.1f, 0.25f);");
        gpBase->mpPlayer->GetHelperLantern()->SetActive(false, false);
        mTestMediaChanged = true; NetLog("[NET/TEST] Media properties changed on host");
    }
    if(mTestMediaChanged && !mTestMediaRemoved && elapsed > 16) {
        auto* sound = world->GetSoundEntity("online_media_fixture_sound"); auto* ps = world->GetParticleSystem("online_media_fixture_particle");
        if(sound) world->DestroySoundEntity(sound); if(ps) world->DestroyParticleSystem(ps);
        gpBase->mpMusicHandler->Stop(0.5f, 7);
        map->RunScript("FadeGlobalSoundVolume(1.0f, 0.25f); FadeGlobalSoundSpeed(1.0f, 0.25f);");
        mTestMediaRemoved = true; NetLog("[NET/TEST] Media fixtures removed on host");
    }
}
void cLuxNetwork::ReplicateLights(cLuxMap* map) {
    cWorld* world = map->GetWorld();
    online::Presentation fog[2];
    fog[0].epoch = fog[1].epoch = mEpoch;
    fog[0].kind = online::PresentationKind::FogColor;
    const cColor color = world->GetFogColor();
    fog[0].values = {{int32_t(color.r * 1000), int32_t(color.g * 1000), int32_t(color.b * 1000), int32_t(color.a * 1000), 0, 0}};
    fog[1].kind = online::PresentationKind::FogProperties;
    fog[1].values = {{int32_t(world->GetFogStart() * 1000), int32_t(world->GetFogEnd() * 1000), int32_t(world->GetFogFalloffExp() * 1000),
        (world->GetFogActive() ? 1 : 0) | (world->GetFogCulling() ? 2 : 0), 0, 0}};
    for(int i = 0; i < 2; ++i) {
        auto bytes = online::EncodePresentation(fog[i]);
        if(!bytes.empty() && bytes != mFogBytes[i] && mSession.SendPresentation(fog[i])) mFogBytes[i] = std::move(bytes);
    }
    unsigned sent = 0;
    cLightListIterator it = map->GetWorld()->GetLightIterator();
    while(it.HasNext()) {
        iLight* light = it.Next();
        if((light->GetName().find("PlayerLantern") == 0 || light->GetName().find("PlayerDarknessAmbient") == 0)) continue; // ownership is swapped on the client
        online::Presentation event; event.epoch = mEpoch; event.kind = online::PresentationKind::Light; event.text[0] = light->GetName();
        const cColor& color = light->GetDiffuseColor(); const float values[] = {color.r, color.g, color.b, color.a};
        for(int i = 0; i < 4; ++i) event.values[i] = int32_t(std::max(0.0f, std::min(65.0f, values[i])) * 1000);
        event.values[4] = int32_t(std::max(0.0f, std::min(100000.0f, light->GetRadius())) * 1000);
        event.values[5] = (light->IsVisible() ? 1 : 0) | (light->IsActive() ? 2 : 0);
        auto bytes = online::EncodePresentation(event);
        auto old = mLightBytes.find(light->GetName());
        if(sent < 64 && !bytes.empty() && (old == mLightBytes.end() || old->second != bytes) && mSession.SendPresentation(event)) {
            mLightBytes[light->GetName()] = std::move(bytes); ++sent;
        }
    }
}
void cLuxNetwork::ApplyPresentation(cLuxMap* map, const online::Presentation& event) {
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_EFFECTS") && event.kind==online::PresentationKind::Message && event.text[0]=="AMNESIA_QA_JUMP_READY") {
        mEffectsTestJumpReady=true;
        mEffectsTestJumpSpawn=cVector3f(event.values[0]/1000.0f,event.values[1]/1000.0f,event.values[2]/1000.0f);
        return;
    }
    switch(event.kind) {
    case online::PresentationKind::GuiSound: {
        cLuxSoundExtraData sound = {};
        if(cString::GetFileExt(event.text[0]).empty() || cString::GetFileExt(event.text[0]) == "snt")
            gpBase->mpHelpFuncs->PlayGuiSoundData(event.text[0], eSoundEntryType_Gui, event.values[0] / 1000.0f,
                eSoundEntityType_Main, true, &sound);
        else sound.mpSoundEntry = gpBase->mpEngine->GetSound()->GetSoundHandler()->PlayGui(event.text[0], false, event.values[0] / 1000.0f);
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA")) {
            NetLog("[NET/TEST] GUI sound event applied on client");
            if(event.text[0] == "ui_lantern_on" || event.text[0] == "ui_lantern_off") {
                if(event.target != 2 || !sound.mpSoundEntry) NetLog("[NET/TEST] Owner lantern audio channel FAILED");
                else mTestLanternSounds.push_back({sound.mpSoundEntry, sound.mpSoundEntry->GetId(), event.text[0], std::chrono::steady_clock::now()});
                if(event.text[0] == "ui_lantern_on") ++mTestLanternOnSounds; else ++mTestLanternOffSounds;
            }
        }
        break;
    }
    case online::PresentationKind::SoundFade: {
        auto it = map->GetWorld()->GetSoundEntityIterator();
        while(it.HasNext()) {
            auto* sound = it.Next(); if(sound->GetName() != event.text[0]) continue;
            const float time = event.values[0] / 1000.0f;
            if(event.values[1]) { if(time > 0) sound->FadeIn(1.0f / time); else sound->Play(event.values[2] != 0); }
            else { if(time > 0) sound->FadeOut(1.0f / time); else sound->Stop(true); }
        }
        break;
    }
    case online::PresentationKind::GlobalVolume:
    case online::PresentationKind::GlobalSpeed: {
        auto* handler = gpBase->mpEngine->GetSound()->GetSoundHandler();
        auto* settings = event.kind == online::PresentationKind::GlobalVolume ? handler->GetGlobalVolumeSettingsHandler() : handler->GetGlobalSpeedSettingsHandler();
        const float goal = event.values[0] / 1000.0f, time = event.values[1] / 1000.0f;
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA") && event.values[1] > 0)
            NetLog(event.kind == online::PresentationKind::GlobalVolume ? "[NET/TEST] Global sound volume fade applied on client" : "[NET/TEST] Global sound speed fade applied on client");
        const float speed = time > 0 ? std::fabs(goal - settings->GetEntry(eLuxGlobalVolumeType_Script, true)->GetVal()) / time : 1000;
        if(event.kind == online::PresentationKind::GlobalVolume) handler->FadeGlobalVolume(goal, speed, eSoundEntryType_World, eLuxGlobalVolumeType_Script, goal == 1);
        else handler->FadeGlobalSpeed(goal, speed, eSoundEntryType_World, eLuxGlobalVolumeType_Script, goal == 1);
        break;
    }
    case online::PresentationKind::SanityGain:
        gpBase->mpEffectHandler->GetSanityGainFlash()->StartForPlayer(gpBase->mpPlayer);
        NetLog("[NET/EFFECT] Objective sanity gain flash applied on client");
        break;
    case online::PresentationKind::Emotion:
        if(event.values[0]) {
            gpBase->mpEffectHandler->GetEmotionFlash()->BeginFadeOut();
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_EFFECTS")) ++mEffectsTestEnds;
        } else {
            gpBase->mpEffectHandler->GetEmotionFlash()->Start(event.text[0], event.text[1], event.text[2]);
            if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_EFFECTS")) { ++mEffectsTestStarts; mEffectsTestAt = std::chrono::steady_clock::now(); }
        }
        break;
    case online::PresentationKind::Shake:
        if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MEDIA")) NetLog("[NET/TEST] Screen shake applied on client");
        gpBase->mpEffectHandler->GetScreenShake()->Start(event.values[0] / 1000.0f, event.values[1] / 1000.0f,
            event.values[2] / 1000.0f, event.values[3] / 1000.0f, 0); break;
    case online::PresentationKind::Hint:
        gpBase->mpHintHandler->Add(event.text[0], cString::UTF8ToWChar(event.text[1]), event.values[0] / 1000.0f);
        if(mSmokeSeconds) NetLog("[NET/TEST] Host hint applied on client: " + event.text[0]);
        break;
    case online::PresentationKind::Fade:
        if(event.values[0]) gpBase->mpEffectHandler->GetFade()->FadeOut(event.values[1] / 1000.0f);
        else gpBase->mpEffectHandler->GetFade()->FadeIn(event.values[1] / 1000.0f);
        break;
    case online::PresentationKind::ImageTrail:
        gpBase->mpEffectHandler->GetImageTrail()->FadeTo(event.values[0] / 1000.0f, event.values[1] / 1000.0f); break;
    case online::PresentationKind::Sepia:
        gpBase->mpEffectHandler->GetSepiaColor()->FadeTo(event.values[0] / 1000.0f, event.values[1] / 1000.0f); break;
    case online::PresentationKind::RadialBlur:
        gpBase->mpEffectHandler->GetRadialBlur()->FadeTo(event.values[0] / 1000.0f, event.values[1] / 1000.0f); break;
    case online::PresentationKind::BlurStart:
        gpBase->mpEffectHandler->GetRadialBlur()->SetBlurStartDist(event.values[0] / 1000.0f); break;
    case online::PresentationKind::Flash:
        gpBase->mpEffectHandler->GetFlash()->Start(event.values[0] / 1000.0f, event.values[1] / 1000.0f, event.values[2] / 1000.0f); break;
    case online::PresentationKind::FogColor:
        map->GetWorld()->SetFogColor(cColor(event.values[0] / 1000.0f, event.values[1] / 1000.0f, event.values[2] / 1000.0f, event.values[3] / 1000.0f)); break;
    case online::PresentationKind::FogProperties:
        map->GetWorld()->SetFogStart(event.values[0] / 1000.0f); map->GetWorld()->SetFogEnd(event.values[1] / 1000.0f);
        map->GetWorld()->SetFogFalloffExp(event.values[2] / 1000.0f); map->GetWorld()->SetFogActive((event.values[3] & 1) != 0);
        map->GetWorld()->SetFogCulling((event.values[3] & 2) != 0); break;
    case online::PresentationKind::Light: {
        iLight* light = map->GetWorld()->GetLight(event.text[0]);
        if(!light || (event.text[0].find("PlayerLantern") == 0 || event.text[0].find("PlayerDarknessAmbient") == 0)) break;
        light->SetFlickerActive(false); // replicate the host's actual flicker rather than a second random timer
        light->SetDiffuseColor(cColor(event.values[0] / 1000.0f, event.values[1] / 1000.0f, event.values[2] / 1000.0f, event.values[3] / 1000.0f));
        light->SetRadius(event.values[4] / 1000.0f); light->SetVisible((event.values[5] & 1) != 0); light->SetActive((event.values[5] & 2) != 0);
        if(!mLoggedLighting) { mLoggedLighting = true; NetLog("[NET/WORLD] Host scripted lighting applied"); }
        break;
    }
    case online::PresentationKind::Voice:
        gpBase->mpEffectHandler->GetPlayVoice()->AddVoice(event.text[0], event.text[1], event.text[2], event.text[3], event.values[5] != 0,
            cVector3f(event.values[0] / 1000.0f, event.values[1] / 1000.0f, event.values[2] / 1000.0f), event.values[3] / 1000.0f, event.values[4] / 1000.0f);
        break;
    case online::PresentationKind::StopVoice:
        gpBase->mpEffectHandler->GetPlayVoice()->StopVoices(event.values[0] / 1000.0f); break;
    case online::PresentationKind::Message:
        if(mSmokeSeconds && event.text[0] == "Online story relay test") NetLog("[NET/TEST] Host story message applied on client");
        gpBase->mpMessageHandler->SetMessage(cString::UTF8ToWChar(event.text[0]), event.values[0] / 1000.0f, 0); break;
    case online::PresentationKind::Note:
    case online::PresentationKind::Diary: {
        cLuxJournal* journal = gpBase->mpJournal;
        cLuxNote* note = NULL; cLuxDiary* diary = NULL; int index = 0;
        if(event.kind == online::PresentationKind::Note) note = journal->AddNote(event.text[0], event.text[1]);
        else diary = journal->AddDiary(event.text[0], event.text[1], index);
        if(event.values[0] > 0 && (note || diary)) {
            journal->SetActivePlayer(gpBase->mpPlayer); journal->SetShowOnBothPlayers(false); journal->SetPauseBothPlayers(false);
            gpBase->mpEngine->GetUpdater()->SetContainer("Journal"); journal->SetForceInstantExit(true);
            if(note) journal->OpenNote(note, event.values[0] >= 2); else journal->OpenDiary(diary, true);
        }
        break;
    }
    }
}
// 07_archives_cellar: reaching PastFirstGate slams the lever gate and locks the
// lever. The host moves one player through, back out, then the partner; the
// original callback (which also wakes waterlurker_2) must wait for both.
// AMNESIA_NET_TEST_LEVERS: il client afferra una leva e una ruota originali e
// le muove con il mouse, attraverso la rete (comandi e azione d'interazione
// come un giocatore vero). L'host misura lo stato reale della leva e l'angolo
// reale della ruota. Zona isolata, come la prova di comparsa.
namespace {
const cVector3f kLeverTestOrigin(100, 30, 100);
const cVector3f kLeverSpot = kLeverTestOrigin + cVector3f(0, .01f, 0);
const cVector3f kWheelSpot = kLeverTestOrigin + cVector3f(3, .01f, 0);
}
void cLuxNetwork::UpdateLeverTest(cLuxMap* map) {
    if(!map || IsClientReplica() || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer2 || mLeverTestStage >= 4) return;
    const auto now = std::chrono::steady_clock::now();
    if(!mLeverTestAt.time_since_epoch().count()) { mLeverTestAt = now; return; }
    const float elapsed = std::chrono::duration<float>(now - mLeverTestAt).count();
    // AMNESIA_NET_TEST_LEVERS_LOCAL: riferimento senza rete, la stessa sequenza
    // eseguita dal giocatore dell'host in locale sulla stessa leva e ruota.
    const bool local = TestFlag("AMNESIA_NET_TEST_LEVERS_LOCAL");
    cLuxPlayer* partner = local ? gpBase->mpPlayer : gpBase->mpPlayer2;
    cLuxPlayer* bystander = local ? gpBase->mpPlayer2 : gpBase->mpPlayer;
    auto place = [&](cLuxPlayer* player, const cVector3f& feet) {
        player->GetCharacterBody()->SetFeetPosition(feet); player->GetCharacterBody()->SetForceVelocity(cVector3f(0,0,0));
    };
    if(mLeverTestStage == 0) {
        if(elapsed < 2) return;
        auto* world = map->GetPhysicsWorld();
        world->SetWorldSize(cVector3f(-200,-100,-200), cVector3f(200,100,200));
        auto* floor = world->CreateBody("online_qa_lever_floor", world->CreateBoxShape(cVector3f(10, .3f, 6), NULL));
        floor->SetMass(0); floor->SetPosition(kLeverTestOrigin + cVector3f(1.5f, -.15f, -1)); floor->SetCollideCharacter(true);
        // Come in 07_archives_cellar (lever_simple01_1): girata di 180° verso
        // il giocatore. Niente muro finto dietro: la maniglia lo attraversava
        // e la leva si incastrava anche per il giocatore locale.
        cMatrixf leverMatrix = cMath::MatrixRotateY(kPif); leverMatrix.SetTranslation(kLeverSpot + cVector3f(0, 1.1f, -.75f));
        map->CreateEntity("online_qa_lever", "entities/gameplay/lever_simple01/lever_simple01.ent", leverMatrix, cVector3f(1));
        map->CreateEntity("online_qa_wheel", "entities/gameplay/valve_iron/valve_iron.ent", cMath::MatrixTranslate(kWheelSpot + cVector3f(0, 1.1f, -.6f)), cVector3f(1));
        place(bystander, kLeverTestOrigin + cVector3f(-3, .01f, 3));
        place(partner, kLeverSpot);
        mLeverTestStage = 1; mLeverTestAt = now; mLeverMaxAngle = mWheelMaxAngle = 0; mLeverLimitSeconds = mLeverGrabSeconds = -1;
        NetLog("[NET/TEST] Lever fixture ready; partner placed at the lever");
        return;
    }
    auto* lever = static_cast<cLuxProp_Lever*>(map->GetEntityByName("online_qa_lever", eLuxEntityType_Prop));
    auto* wheel = static_cast<cLuxProp_Wheel*>(map->GetEntityByName("online_qa_wheel", eLuxEntityType_Prop));
    if(!lever || !wheel || !lever->GetBodyNum() || !wheel->GetBodyNum()) { NetLog("[NET/TEST] FAILED lever fixture missing"); mLeverTestStage = 4; return; }
    if(local && (mLeverTestStage == 1 || mLeverTestStage == 2)) DriveLeverLocally(map, partner, mLeverTestStage, gpBase->mpEngine->GetStepSize());
    if(mLeverTestStage == 1) {
        // Angolo percorso dalla leva: rotazione della maniglia (il corpo con
        // massa; il primo è la base fissa) rispetto all'inizio.
        iPhysicsBody* handle = lever->GetBody(0);
        for(int i = 0; i < lever->GetBodyNum(); ++i) if(lever->GetBody(i)->GetMass() > 0) { handle = lever->GetBody(i); break; }
        const cMatrixf current = handle->GetLocalMatrix();
        // Dalla presa in poi: appena creata la leva si assesta da sola.
        if(!mLeverHasStart && mLeverGrabSeconds >= 0) { mLeverStartUp = current.GetUp(); mLeverStartForward = current.GetForward(); mLeverHasStart = true; }
        if(!mLeverHasStart) { mLeverStartUp = current.GetUp(); mLeverStartForward = current.GetForward(); }
        const float dotUp = std::max(-1.0f, std::min(1.0f, cMath::Vector3Dot(mLeverStartUp, current.GetUp())));
        const float dotForward = std::max(-1.0f, std::min(1.0f, cMath::Vector3Dot(mLeverStartForward, current.GetForward())));
        mLeverMaxAngle = std::max(mLeverMaxAngle, cMath::ToDeg(std::max(std::acos(dotUp), std::acos(dotForward))));
        if(partner->GetCurrentState() == eLuxPlayerState_InteractLever && mLeverGrabSeconds < 0) {
            mLeverGrabSeconds = elapsed;
            NetLog("[NET/TEST] Partner grabbed the lever body=" + std::string(partner->GetBodyInFocus() ? partner->GetBodyInFocus()->GetName() : "none") +
                " mass=" + std::to_string(partner->GetBodyInFocus() ? partner->GetBodyInFocus()->GetMass() : -1.0f) + " stuck=" + std::to_string(lever->GetStuckState()));
        }
        if(int(elapsed * 2) != int(mLeverLastLog * 2)) {
            mLeverLastLog = elapsed;
            NetLog("[NET/TEST] Lever t=" + std::to_string(elapsed) + " state=" + std::to_string(lever->GetLeverState()) + " angle_deg=" + std::to_string(mLeverMaxAngle) +
                " handle_enabled=" + std::to_string(handle->GetEnabled() ? 1 : 0) + " partner_state=" + std::to_string(int(partner->GetCurrentState())) +
                " partner_look_mul=" + std::to_string(partner->GetLookSpeedMul()));
        }
        if(lever->GetLeverState() != 0 && mLeverGrabSeconds >= 0 && !(mLeverStatesSeen & (lever->GetLeverState() > 0 ? 2 : 1))) {
            mLeverStatesSeen |= lever->GetLeverState() > 0 ? 2 : 1;
            NetLog("[NET/TEST] Lever end reached state=" + std::to_string(lever->GetLeverState()) + " seconds_after_grab=" + std::to_string(elapsed - mLeverGrabSeconds));
        }
        if(lever->GetLeverState() != 0 && mLeverLimitSeconds < 0 && mLeverGrabSeconds >= 0) {
            mLeverLimitSeconds = elapsed - mLeverGrabSeconds;
            NetLog("[NET/TEST] Lever reached its limit state=" + std::to_string(lever->GetLeverState()) + " seconds_after_grab=" + std::to_string(mLeverLimitSeconds));
        }
        if(elapsed > 10) {
            const bool ok = mLeverGrabSeconds >= 0 && mLeverLimitSeconds >= 0;
            NetLog(std::string("[NET/TEST] ") + (ok ? "PASS" : "FAILED") + " client pulled the lever ends_reached=" + std::to_string((mLeverStatesSeen & 1) + ((mLeverStatesSeen >> 1) & 1)) + " grabbed=" + (mLeverGrabSeconds >= 0 ? "1" : "0") +
                " limit_seconds=" + std::to_string(mLeverLimitSeconds) + " max_angle_deg=" + std::to_string(mLeverMaxAngle));
            place(partner, kWheelSpot); mLeverTestStage = 2; mLeverTestAt = now; mWheelStartAngle = wheel->GetAngle(); mWheelGrabSeconds = -1;
            NetLog("[NET/TEST] Partner placed at the wheel");
        }
        return;
    }
    if(mLeverTestStage == 2) {
        mWheelMaxAngle = std::max(mWheelMaxAngle, std::fabs(cMath::ToDeg(wheel->GetAngle() - mWheelStartAngle)));
        if(partner->GetCurrentState() == eLuxPlayerState_InteractWheel && mWheelGrabSeconds < 0) {
            mWheelGrabSeconds = elapsed; NetLog("[NET/TEST] Partner grabbed the wheel");
        }
        if(elapsed > 10) {
            const bool ok = mWheelGrabSeconds >= 0 && mWheelMaxAngle >= 45;
            NetLog(std::string("[NET/TEST] ") + (ok ? "PASS" : "FAILED") + " client turned the wheel grabbed=" + (mWheelGrabSeconds >= 0 ? "1" : "0") +
                " max_angle_deg=" + std::to_string(mWheelMaxAngle));
            mLeverTestStage = 4;
        }
    }
}
// Host, riferimento locale: la stessa sequenza del client (mira, presa,
// movimento del mouse, rilascio) applicata direttamente al giocatore.
void cLuxNetwork::DriveLeverLocally(cLuxMap* map, cLuxPlayer* player, unsigned target, float timeStep) {
    if(target != unsigned(mLeverDriveTarget)) { mLeverDriveTarget = int(target); mLeverDriveTime = 0; mLeverDriveHeld = false; mLeverDriveDone = false; }
    if(mLeverDriveDone) return;
    auto* camera = player->GetCamera();
    iLuxEntity* entity = map->GetEntityByName(target == 1 ? "online_qa_lever" : "online_qa_wheel");
    auto* prop = entity && entity->GetEntityType() == eLuxEntityType_Prop ? static_cast<iLuxProp*>(entity) : NULL;
    if(!prop || !prop->GetBodyNum()) return;
    if(!mLeverDriveHeld) {
        iPhysicsBody* handle = prop->GetBody(prop->GetBodyNum()-1);
        const cVector3f aim = cMath::MatrixMul(handle->GetLocalMatrix(), handle->GetMassCentre()) - camera->GetPosition();
        camera->SetYaw(std::atan2(-aim.x, -aim.z)); camera->SetPitch(std::atan2(aim.y, std::sqrt(aim.x*aim.x + aim.z*aim.z)));
        player->GetCharacterBody()->SetYaw(camera->GetYaw());
        mLeverDriveTime += timeStep;
        if(mLeverDriveTime >= .5f) {
            if(player->GetCurrentState() == eLuxPlayerState_Normal) player->GetCurrentStateData()->Update(0);
            player->DoAction(eLuxPlayerAction_Interact, true); mLeverDriveHeld = true; mLeverDriveTime = 0;
            NetLog(std::string("[NET/TEST] Host (local reference) grabbing the ") + (target == 1 ? "lever" : "wheel") +
                " focus=" + (player->GetEntityInFocus() ? player->GetEntityInFocus()->GetName() : "none") +
                " body=" + (player->GetBodyInFocus() ? player->GetBodyInFocus()->GetName() : "none") +
                " camera_y=" + std::to_string(camera->GetPosition().y) + " pitch=" + std::to_string(camera->GetPitch()) + " state=" + std::to_string(int(player->GetCurrentState())));
        }
    } else if(mLeverDriveTime < 6.4f) {
        mLeverDriveTime += timeStep;
        const float t = mLeverDriveTime, amount = .02f;
        float dx = 0, dy = 0;
        if(target == 1) dy = t < 3.2f ? -amount : amount;
        else { dx = amount * std::cos(t * 4); dy = amount * std::sin(t * 4); }
        player->AddYaw(dx); player->AddPitch(dy);
    } else {
        player->DoAction(eLuxPlayerAction_Interact, false); mLeverDriveHeld = false; mLeverDriveDone = true;
    }
}
// Client: mira, afferra e muove il mouse come un giocatore, in base al punto
// in cui l'host lo ha messo (leva o ruota). Sostituisce l'input reale.
bool cLuxNetwork::UpdateLeverDrive(float timeStep) {
    cLuxPlayer* player = gpBase->mpPlayer;
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(!map || !player || !player->GetCharacterBody()) return true;
    const cVector3f feet = player->GetCharacterBody()->GetFeetPosition();
    auto near = [&](const cVector3f& spot) { cVector3f d = feet - spot; d.y = 0; return d.Length() < .5f; };
    const int target = near(kLeverSpot) ? 1 : near(kWheelSpot) ? 2 : 0;
    if(target != mLeverDriveTarget) {
        if(mLeverDriveHeld) { SendLocalAction(online::PlayerAction::Interact, false); mLeverDriveHeld = false; }
        mLeverDriveTarget = target; mLeverDriveTime = 0; mLeverDriveDone = false;
    }
    online::PlayerInput input; auto* camera = player->GetCamera();
    if(target && !mLeverDriveDone) {
        iLuxEntity* entity = map->GetEntityByName(target == 1 ? "online_qa_lever" : "online_qa_wheel");
        auto* prop = entity && entity->GetEntityType() == eLuxEntityType_Prop ? static_cast<iLuxProp*>(entity) : NULL;
        if(!mLeverDriveHeld) {
            // Mira alla maniglia (l'ultimo corpo) finché l'oggetto non è
            // arrivato e la mira è rimasta stabile per mezzo secondo.
            // Come un giocatore fermo davanti all'oggetto: dopo il
            // teletrasporto il corpo deve essere arrivato anche in altezza.
            const cVector3f spot = target == 1 ? kLeverSpot : kWheelSpot;
            if(!prop || !prop->GetBodyNum() || (feet - spot).Length() > .1f) { mLeverDriveTime = 0; }
            else {
                // Al centro della maniglia (l'ultimo corpo): l'origine del corpo
                // è sul perno, vicino alla base fissa.
                // Centro di massa dalla matrice del corpo: la posa che si vede a
                // schermo (il volume di una copia mossa dalla rete può essere vecchio).
                iPhysicsBody* handle = prop->GetBody(prop->GetBodyNum()-1);
                const cVector3f aim = cMath::MatrixMul(handle->GetLocalMatrix(), handle->GetMassCentre()) - camera->GetPosition();
                camera->SetYaw(std::atan2(-aim.x, -aim.z)); camera->SetPitch(std::atan2(aim.y, std::sqrt(aim.x*aim.x + aim.z*aim.z)));
                mLeverDriveTime += timeStep;
                if(mLeverDriveTime >= .5f) {
                    SendLocalAction(online::PlayerAction::Interact, true); mLeverDriveHeld = true; mLeverDriveTime = 0;
                    NetLog(std::string("[NET/TEST] Client grabbing the ") + (target == 1 ? "lever" : "wheel") + " camera_y=" + std::to_string(camera->GetPosition().y) +
                        " pitch=" + std::to_string(camera->GetPitch()));
                }
            }
        } else if(mLeverDriveTime < 6.4f) {
            // Quello che il giocatore vede: rotazione della copia locale.
            // La rotazione massima fra tutti i corpi della copia (la parte
            // mobile non è sempre l'ultimo corpo: nella ruota c'è il supporto).
            if(prop && prop->GetBodyNum()) {
                if(!mLeverSeenStartValid) {
                    mLeverSeenStarts.clear();
                    for(int i = 0; i < prop->GetBodyNum(); ++i) mLeverSeenStarts.push_back(prop->GetBody(i)->GetLocalMatrix());
                    mLeverSeenStartValid = true; mLeverSeenMax = 0;
                }
                for(int i = 0; i < prop->GetBodyNum() && size_t(i) < mLeverSeenStarts.size(); ++i) {
                    const cMatrixf seen = prop->GetBody(i)->GetLocalMatrix();
                    const float up = std::max(-1.0f, std::min(1.0f, cMath::Vector3Dot(mLeverSeenStarts[i].GetUp(), seen.GetUp())));
                    const float right = std::max(-1.0f, std::min(1.0f, cMath::Vector3Dot(mLeverSeenStarts[i].GetRight(), seen.GetRight())));
                    mLeverSeenMax = std::max(mLeverSeenMax, cMath::ToDeg(std::max(std::acos(up), std::acos(right))));
                }
            }
            // Movimento del mouse di un giocatore: 0,02 rad per tick (circa
            // 1,2 rad/s). Leva: giù per 3,2 s, poi su; ruota: cerchi.
            mLeverDriveTime += timeStep;
            const float t = mLeverDriveTime, amount = .02f;
            float dx = 0, dy = 0;
            if(target == 1) dy = t < 3.2f ? -amount : amount; // tira giù fino in fondo, poi su
            else { dx = amount * std::cos(t * 4); dy = amount * std::sin(t * 4); }
            input.lookYaw = int16_t(dx * 10000); input.lookPitch = int16_t(dy * 10000);
        } else {
            SendLocalAction(online::PlayerAction::Interact, false); mLeverDriveHeld = false; mLeverDriveDone = true;
            NetLog(std::string("[NET/TEST] Client released the ") + (target == 1 ? "lever" : "wheel"));
            // Leva: almeno 20° visti sul client; ruota: almeno 45°.
            const bool seenOk = mLeverSeenMax >= (target == 1 ? 20.0f : 45.0f);
            NetLog(std::string("[NET/TEST] ") + (seenOk ? "PASS" : "FAILED") + " client sees the " + (target == 1 ? "lever" : "wheel") +
                " move max_angle_deg=" + std::to_string(mLeverSeenMax));
            mLeverSeenStartValid = false;
        }
    }
    const float yaw = camera->GetYaw();
    input.yaw = int16_t(std::atan2(std::sin(yaw), std::cos(yaw)) * 1000);
    input.pitch = int16_t(std::max(-1.571f, std::min(1.571f, camera->GetPitch())) * 1000);
    SendLocalInput(input, timeStep);
    return true;
}
void cLuxNetwork::UpdateGateTest(cLuxMap* map) {
    if(!map || IsClientReplica() || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer2 || mGateTestStage >= 4) return;
    auto* area = static_cast<iLuxArea*>(map->GetEntityByName("PastFirstGate", eLuxEntityType_Area));
    iLuxEntity* lurker = map->GetEntityByName("waterlurker_2");
    if(!area || !lurker) { NetLog("[NET/TEST] FAILED gate fixture missing on " + map->GetName()); mGateTestStage = 4; return; }
    const auto now = std::chrono::steady_clock::now();
    auto place = [&](cLuxPlayer* player, const cVector3f& feet) {
        player->GetCharacterBody()->SetFeetPosition(feet); player->GetCharacterBody()->SetForceVelocity(cVector3f(0,0,0));
    };
    auto elapsed = [&](int ms) { return now - mGateTestAt >= std::chrono::milliseconds(ms); };
    if(mGateTestStage == 0) {
        if(!mGateTestAt.time_since_epoch().count()) { mGateTestAt = now; return; }
        if(!elapsed(2000)) return;
        if(lurker->IsActive()) { NetLog("[NET/TEST] FAILED gate fixture already triggered"); mGateTestStage = 4; return; }
        mGateTestHostStart = gpBase->mpPlayer->GetCharacterBody()->GetFeetPosition();
        place(gpBase->mpPlayer, area->GetPosition());
        NetLog("[NET/TEST] Host player moved past the lever gate; partner still outside");
        mGateTestStage = 1; mGateTestAt = now;
    } else if(mGateTestStage == 1 && elapsed(1500)) {
        if(lurker->IsActive()) { NetLog("[NET/TEST] FAILED gate closed while the partner was outside"); mGateTestStage = 4; return; }
        NetLog("[NET/TEST] PASS gate stays open while one player is past it");
        place(gpBase->mpPlayer, mGateTestHostStart); // leaving again must not reset the latch
        mGateTestStage = 2; mGateTestAt = now;
    } else if(mGateTestStage == 2 && elapsed(500)) {
        place(gpBase->mpPlayer2, area->GetPosition());
        NetLog("[NET/TEST] Partner moved past the lever gate");
        mGateTestStage = 3; mGateTestAt = now;
    } else if(mGateTestStage == 3 && elapsed(1500)) {
        NetLog(lurker->IsActive() ? "[NET/TEST] PASS gate closes once both players passed" : "[NET/TEST] FAILED gate did not close after both players passed");
        mGateTestStage = 4;
    }
}
// 80 props move every tick, more than the 64 entity messages a tick may carry,
// and one late-created target moves once. Before fair scheduling the target sat
// behind the movers forever (the "door still closed on the client" report).
void cLuxNetwork::UpdateFairnessTest(cLuxMap* map) {
    if(!map || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || mFairnessTestStage >= 3) return;
    const auto now = std::chrono::steady_clock::now();
    if(!mFairnessTestAt.time_since_epoch().count()) { mFairnessTestAt = now; return; }
    const cVector3f origin(100, 30, 100);
    if(IsClientReplica()) {
        iLuxEntity* target = map->GetEntityByName("online_qa_fair_target");
        if(!target || target->GetEntityType() != eLuxEntityType_Prop || !static_cast<iLuxProp*>(target)->GetBodyNum()) return;
        const float x = static_cast<iLuxProp*>(target)->GetBody(0)->GetLocalPosition().x;
        if(mFairnessTestStage == 0) { mFairnessTestStage = 1; return; }
        if(x > origin.x + 1.5f) { NetLog("[NET/TEST] PASS late entity update reached client despite 80 moving props"); mFairnessTestStage = 3; }
        return;
    }
    const float elapsed = std::chrono::duration<float>(now - mFairnessTestAt).count();
    if(mFairnessTestStage == 0 && elapsed > 2) {
        map->GetPhysicsWorld()->SetWorldSize(cVector3f(-200,-100,-200), cVector3f(200,100,200));
        for(int i = 0; i < 80; ++i)
            map->CreateEntity("online_qa_fair_mover_" + std::to_string(i), "entities/item/tinderbox/tinderbox.ent",
                cMath::MatrixTranslate(origin + cVector3f(float(i % 10) * 0.5f, 2, float(i / 10) * 0.5f)), cVector3f(1));
        map->CreateEntity("online_qa_fair_target", "entities/item/tinderbox/tinderbox.ent", cMath::MatrixTranslate(origin), cVector3f(1));
        mFairnessTestStage = 1;
        NetLog("[NET/TEST] Host spawned 80 movers and a late target");
    }
    if(mFairnessTestStage >= 1) {
        for(int i = 0; i < 80; ++i) {
            auto* mover = static_cast<iLuxProp*>(map->GetEntityByName("online_qa_fair_mover_" + std::to_string(i), eLuxEntityType_Prop));
            if(mover && mover->GetBodyNum())
                mover->GetBody(0)->SetPosition(origin + cVector3f(float(i % 10) * 0.5f, 2 + 0.3f * std::sin(elapsed * 6 + i), float(i / 10) * 0.5f));
        }
    }
    if(mFairnessTestStage == 1 && elapsed > 6) {
        auto* target = static_cast<iLuxProp*>(map->GetEntityByName("online_qa_fair_target", eLuxEntityType_Prop));
        if(target && target->GetBodyNum()) {
            target->GetBody(0)->SetMass(0); target->GetBody(0)->SetPosition(origin + cVector3f(2, 0, 0));
            NetLog("[NET/TEST] Host moved the late target once");
        }
        mFairnessTestStage = 2;
    }
}
// A late-campaign save had item images like graphics/item/graphics/item/.../lantern.tga,
// one prefix per save/load, and bucket_of_tar_1 kept an authoring drive path; either
// made the inventory unsendable and the client never finished loading 22_torture_chancel.
void cLuxNetwork::UpdateItemPathTest() {
    if(!mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || mItemPathTestDone) return;
    const tString expected = "graphics/item/lantern.tga";
    if(IsClientReplica()) {
        cLuxInventory_Item* item = gpBase->mpInventory->GetItem("online_qa_inflated_item");
        cLuxInventory_Item* tar = gpBase->mpInventory->GetItem("online_qa_bucket_of_tar");
        if(!item || !tar) return;
        mItemPathTestDone = true;
        NetLog(std::string("[NET/TEST] ") + (item->GetImageName() == expected ? "PASS" : "FAILED") +
            " client received item restored from an inflated save path; image=" + item->GetImageName());
        NetLog(std::string("[NET/TEST] ") + (tar->GetImageName() == "graphics/item/bucket_of_tar.tga" ? "PASS" : "FAILED") +
            " client received the original bucket_of_tar.ent item; image=" + tar->GetImageName());
        return;
    }
    // The original entity, given as 22_torture_chancel's GiveItemFromFile does.
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(!map) return;
    map->ResetLatestEntity();
    map->CreateEntity("online_qa_bucket_of_tar", "bucket_of_tar.ent", cMatrixf::Identity, 1);
    iLuxEntity* tar = map->GetLatestEntity();
    if(tar && tar->GetEntityType() == eLuxEntityType_Prop && static_cast<iLuxProp*>(tar)->GetPropType() == eLuxPropType_Item) {
        auto* prop = static_cast<cLuxProp_Item*>(tar);
        gpBase->mpInventory->AddItem("online_qa_bucket_of_tar", prop->GetItemType(), prop->GetSubItemTypeName(), prop->GetImageFile(), prop->GetAmount(), "", "");
        NetLog("[NET/TEST] Host gave bucket_of_tar.ent with ImageFile=" + prop->GetImageFile());
    }
    if(tar) map->DestroyEntity(tar);
    tString inflated;
    for(int i = 0; i < 80; ++i) inflated += "graphics/item/";
    // As saved for bucket_of_tar_1, whose .ent carries Frictional's authoring path.
    cLuxInventory_Item* item = gpBase->mpInventory->AddItem("online_qa_inflated_item", eLuxItemType_Puzzle, "online_qa_inflated_item",
        inflated + "F:/Amnesia/redist/graphics/item/lantern.tga", 0, "", "");
    mItemPathTestDone = true;
    NetLog(std::string("[NET/TEST] Host restored item from an inflated save path; image=") + (item ? item->GetImageName() : tString("none")));
}
// A real door swung by physics, as scripts open doors with an impulse. It sits
// outside the map so only its hinge touches it; both roles extend the physics
// world to it because the client simulates replicated props too.
void cLuxNetwork::UpdateDoorTest(cLuxMap* map) {
    if(!map || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || mDoorTestStage == 5) return;
    iPhysicsWorld* physics = map->GetPhysicsWorld();
    if(mDoorTestStage == 0) {
        const cVector3f min = physics->GetWorldSizeMin(), max = physics->GetWorldSizeMax();
        mDoorTestOrigin = cVector3f(max.x + 20, (min.y + max.y) / 2, max.z + 20);
        physics->SetWorldSize(min, max + cVector3f(40, 0, 40));
        mDoorTestStage = 1;
    }
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - mSmokeStart).count();
    auto* door = static_cast<cLuxProp_SwingDoor*>(map->GetEntityByName("online_qa_physics_door", eLuxEntityType_Prop));
    if(door && door->GetPropType() != eLuxPropType_SwingDoor) door = NULL;
    // Body 0 is the static frame; the leaf is the body with mass.
    iPhysicsBody* leaf = NULL;
    for(int i = 0; door && i < door->GetBodyNum(); ++i) if(door->GetBody(i)->GetMass() > 0) leaf = door->GetBody(i);
    if(!leaf) door = NULL;
    auto yaw = [&]() { const cVector3f r = leaf->GetLocalMatrix().GetRight(); return cMath::ToDeg(std::atan2(r.z, r.x)); };
    auto push = [&](float direction) { leaf->AddImpulse(leaf->GetLocalMatrix().GetForward() * leaf->GetMass() * 0.3f * direction); };
    if(IsClientReplica()) {
        // The resting pose is read before the host breaks the door at 30 s.
        if(door && elapsed >= 26 && elapsed < 27) mDoorTestYaw = yaw();
        // The debris has LifeLength 4: the host removes it again after four seconds.
        if(map->GetEntityByName("online_qa_physics_door_broken")) mDoorTestDebrisSeen = true;
        if(elapsed < 45) return;
        if(door) NetLog("[NET/TEST] Door pose role=client yaw=" + std::to_string(mDoorTestYaw) + " closed=" + std::to_string(door->GetClosed() ? 1 : 0));
        else NetLog("[NET/TEST] FAILED door: replica missing");
        if(door) NetLog("[NET/TEST] Door broken role=client broken=" + std::to_string(door->IsBroken() ? 1 : 0) + " leaf_active=" + std::to_string(leaf->IsActive() ? 1 : 0) +
            " debris=" + std::to_string(mDoorTestDebrisSeen ? 1 : 0));
        mDoorTestStage = 5;
        return;
    }
    if(mDoorTestStage == 1 && elapsed >= 15) {
        map->CreateEntity("online_qa_physics_door", "entities/door/castle/castle.ent", cMath::MatrixTranslate(mDoorTestOrigin), cVector3f(1));
        mDoorTestStage = 2;
    } else if(mDoorTestStage == 2 && elapsed >= 17 && door) {
        // As the maps' scripts do: unlatch and disable auto-close, then push
        // for a moment (the sticky hinge stops a single impulse at the frame).
        mDoorTestYaw = yaw(); mDoorTestPushAt = elapsed;
        door->SetDisableAutoClose(true); door->SetClosed(false, false);
        NetLog("[NET/TEST] Host pushing physics door closed_yaw=" + std::to_string(mDoorTestYaw));
        mDoorTestStage = 3;
    } else if(mDoorTestStage == 3 && door) {
        if(elapsed - mDoorTestPushAt < 0.6f) push(mDoorTestDirection);
        else if(elapsed - mDoorTestPushAt >= 4) {
            // The hinge opens one way only; if the push hit the stop, push back.
            if(std::fabs(yaw() - mDoorTestYaw) < 15 && mDoorTestDirection > 0) {
                mDoorTestDirection = -1; mDoorTestPushAt = elapsed;
                NetLog("[NET/TEST] Host pushing physics door the other way");
            } else mDoorTestStage = 4;
        }
    } else if(mDoorTestStage == 4 && elapsed >= 26 && door) {
        NetLog("[NET/TEST] Door pose role=host yaw=" + std::to_string(yaw()) + " closed=" + std::to_string(door->GetClosed() ? 1 : 0) +
            " opened_deg=" + std::to_string(std::fabs(yaw() - mDoorTestYaw)));
        mDoorTestStage = 6;
    } else if(mDoorTestStage == 6 && elapsed >= 30 && door) {
        // As a grunt bashing it or SetPropHealth(door, 0) in a map script.
        door->SetHealth(0);
        NetLog("[NET/TEST] Door broken role=host broken=" + std::to_string(door->IsBroken() ? 1 : 0) + " leaf_active=" + std::to_string(leaf->IsActive() ? 1 : 0) +
            " debris=" + std::to_string(map->GetEntityByName("online_qa_physics_door_broken") ? 1 : 0));
        mDoorTestStage = 5;
    }
}
// Field diagnostics for client reports that the local tests do not reproduce
// (2026-10-22: rubber banding at 40-50 ms, own lantern on but not lighting).
// Both are rate-limited and describe the state, so the next hpl.log shows why.
void cLuxNetwork::DiagnoseLocalReplica(const online::PlayerState& local, std::chrono::steady_clock::time_point now) {
    const cVector3f feet(local.x / 1000.0f, local.y / 1000.0f, local.z / 1000.0f);
    const bool movable = (local.flags & 4) && !(local.flags & (8 | 16 | 32));
    if(mHasLastLocalFeet && movable) {
        const float dx = feet.x - mLastLocalFeet.x, dz = feet.z - mLastLocalFeet.z;
        const float jump = std::sqrt(dx * dx + dz * dz);
        // Running covers ~0.08 m per frame; over 0.35 m in a frame is a visible
        // snap. Teleports beyond 5 m are intended and snap by design.
        if(jump > 0.35f && jump < 5.0f && now - mLastSnapLogAt >= std::chrono::seconds(2)) {
            mLastSnapLogAt = now;
            const auto poseAge = mSnapshots.empty() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(now - mSnapshots.back().received).count();
            NetLog("[NET/DIAG] Local position snapped " + std::to_string(int(jump * 1000)) + " mm; pending_inputs=" +
                std::to_string(mPrediction.Pending()) + " rtt_ms=" + std::to_string(mSession.Ping()) +
                " pose_age_ms=" + std::to_string(poseAge) + " vertical_pending=" + std::to_string(mVerticalPrediction.Pending() ? 1 : 0));
        }
    }
    mLastLocalFeet = feet; mHasLastLocalFeet = true;

    auto* lantern = gpBase->mpPlayer->GetHelperLantern();
    if(!lantern->IsActive()) { mLanternDarkSince = {}; return; }
    auto* light = lantern->GetLight();
    auto* hands = gpBase->mpPlayer->GetHands();
    auto* held = hands ? hands->GetCurrentHandObject() : NULL;
    const bool lit = light && light->GetDiffuseColor().r > 0.01f;
    const bool inHand = held && held->GetName() == "lantern";
    if(lit && inHand) { mLanternDarkSince = {}; return; }
    if(!mLanternDarkSince.time_since_epoch().count()) { mLanternDarkSince = now; return; }
    if(now - mLanternDarkSince < std::chrono::milliseconds(1500) || now - mLanternDiagAt < std::chrono::seconds(10)) return;
    mLanternDiagAt = now;
    NetLog("[NET/DIAG] Own lantern active but lit=" + std::to_string(lit) + " in_hand=" + std::to_string(inHand) +
        " held=" + (held ? held->GetName() : std::string("none")) + " light_r=" + std::to_string(light ? light->GetDiffuseColor().r : -1) +
        " oil=" + std::to_string(gpBase->mpPlayer->GetLampOil()) + " state=" + std::to_string(int(gpBase->mpPlayer->GetCurrentState())) +
        " player_active=" + std::to_string(gpBase->mpPlayer->IsActive()));
}
// The host's map load freezes the client's world; say so instead of leaving a
// still picture that looks like a crash. Also covers a host that stopped
// sending without a load notice. The client's own load has its load screen.
void cLuxNetwork::OnDraw(float afFrameTime) {
    if(!IsClientReplica() || !mSession.Connected() || !gpBase->mpGameHudSet || !gpBase->mpDefaultFont ||
        !gpBase->mpMapHandler->GetCurrentMap()) return;
    const wchar_t* text = NULL;
    if(mSession.Loading()) text = _W("L'host sta caricando la mappa...");
    else if(mSession.ApplicationAgeMs() > 1500) text = _W("In attesa dell'host...");
    if(!text) return;
    gpBase->mpGameHudSet->DrawFont(gpBase->mpDefaultFont, cVector3f(400, 540, 30), cVector2f(18, 18), cColor(0.9f, 0.9f),
        eFontAlign_Center, eGuiMaterial_FontNormal, text);
    if(mSmokeSeconds && !mTestNoticeLogged[mSession.Loading() ? 0 : 1]) {
        mTestNoticeLogged[mSession.Loading() ? 0 : 1] = true;
        NetLog(mSession.Loading() ? "[NET/TEST] Client showed host loading notice" : "[NET/TEST] Client showed waiting for host notice");
    }
}
// Regressions for the 2026-10-21 report. The host starts a real flashback (the
// client must show its sepia) and exercises the GUI use-after-free paths that
// used to crash in cGuiSet::RenderClipRegion; the PowerShell side also requires
// the GuiSet warnings that prove the dead elements were skipped.
void cLuxNetwork::UpdateFlashbackTest(cLuxMap* map) {
    if(!map || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer2 || mFlashbackTestStage >= 3) return;
    const auto now = std::chrono::steady_clock::now();
    if(!mFlashbackTestAt.time_since_epoch().count()) { mFlashbackTestAt = now; return; }
    if(IsClientReplica()) {
        if(gpBase->mpEffectHandler->GetSepiaColor()->GetAmount() > 0.5f) {
            NetLog("[NET/TEST] PASS flashback sepia applied on client");
            mFlashbackTestStage = 3;
        }
        return;
    }
    cGui* gui = gpBase->mpEngine->GetGui();
    cGuiSet* hud = gpBase->mpGameHudSet;
    if(mFlashbackTestStage == 0) {
        if(now - mFlashbackTestAt < std::chrono::seconds(2)) return;
        // 1. A caller that keeps drawing after DestroyGfx (drawn below for 30 frames).
        mTestDeadGfx = gui->CreateGfxFilledRect(cColor(1,1), eGuiMaterial_Alpha);
        hud->DrawGfx(mTestDeadGfx, cVector3f(10,10,5), cVector2f(4,4), cColor(1,1));
        gui->DestroyGfx(mTestDeadGfx);
        // 2. A font destroyed after its text was queued: glyphs die before Render.
        auto* fonts = gpBase->mpEngine->GetResources()->GetFontManager();
        if(iFontData* font = fonts->CreateFontData("viewer.fnt")) {
            hud->DrawFont(font, cVector3f(10,30,5), cVector2f(12,12), cColor(1,1), _W("online font regression"));
            fonts->Destroy(font);
        }
        // 3. The held item is removed under the use-item state (partner consumed it).
        cLuxPlayer* holder = gpBase->mpPlayer;
        cLuxInventory_Item* item = gpBase->mpInventory->AddItem("online_test_held_item", eLuxItemType_Puzzle, "", "potion_health.tga", 0, "", "");
        if(item) {
            cLuxPlayerStateVars::SetupUseItem(item);
            holder->ChangeState(eLuxPlayerState_UseItem);
            gpBase->mpInventory->RemoveItem(item);
            NetLog(holder->GetCurrentState() == eLuxPlayerState_UseItem ? "[NET/TEST] FAILED held item removed while still in use" :
                "[NET/TEST] PASS removing a held item returns its holder to normal");
        } else NetLog("[NET/TEST] FAILED held item fixture");
        // 4. A real flashback from the campaign data.
        holder->GetHelperFlashback()->Start("CH01L02_JourneyToSanctum01.flash", "");
        NetLog("[NET/TEST] Host flashback started");
        mFlashbackTestStage = 1; mTestDeadGfxFrames = 0;
    } else if(mFlashbackTestStage == 1) {
        // The deferred delete has run after the first frame: these draws reach a
        // freed element unless DrawGfx rejects it.
        hud->DrawGfx(mTestDeadGfx, cVector3f(10,10,5), cVector2f(4,4), cColor(1,1));
        if(++mTestDeadGfxFrames >= 30) {
            NetLog("[NET/TEST] PASS destroyed GUI element drawn for 30 frames without crashing");
            mTestDeadGfx = NULL; mFlashbackTestStage = 3;
        }
    }
}
void cLuxNetwork::UpdateStairsTest(cLuxMap* map,float dt) {
    if(!map || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer2) return;
    if(!mStairsBuilt) {
        auto* world=map->GetPhysicsWorld();
        world->SetWorldSize(cVector3f(-200,-100,-200),cVector3f(200,100,200));
        auto box=[&](const char* name,cVector3f size,cVector3f position) {
            auto* shape=world->CreateBoxShape(size,NULL);
            auto* body=world->CreateBody(name,shape);
            body->SetMass(0); body->SetPosition(position); body->SetCollideCharacter(true);
            if(auto* material=world->GetMaterialFromName("Glass")) body->SetMaterial(material);
        };
        box("online_qa_stair_floor",cVector3f(10,.3f,20),mStairsOrigin+cVector3f(0,-.15f,-3));
        for(int i=1;i<=8;++i) {
            const std::string name="online_qa_stair_"+std::to_string(i);
            box(name.c_str(),cVector3f(2,i*.15f,.5f),mStairsOrigin+cVector3f(0,i*.075f,-.75f-i*.5f));
        }
        box("online_qa_stair_landing",cVector3f(2,1.2f,2),mStairsOrigin+cVector3f(0,.6f,-6));
        mStairsBuilt=true;
        if(!IsClientReplica()) {
            auto* host=gpBase->mpPlayer; auto* peer=gpBase->mpPlayer2;
            host->GetCharacterBody()->SetFeetPosition(mStairsOrigin+cVector3f(3,0,0));
            peer->GetCharacterBody()->SetFeetPosition(mStairsOrigin);
            peer->GetCharacterBody()->SetForceVelocity(cVector3f(0,0,0));
            peer->GetCharacterBody()->SetYaw(0); peer->GetCamera()->SetYaw(0);
            cLuxSoundExtraData data; data.mfVolume=99; data.mfMaxDistance=99;
            const bool played=gpBase->mpHelpFuncs->PlayGuiSoundData("online_qa_missing_step",eSoundEntryType_World,1,eSoundEntityType_Main,true,&data);
            const bool safe=!played && data.mfVolume==0 && data.mfMaxDistance==0 && data.mpSoundEntry==NULL;
            NetLog(std::string("[NET/TEST] ")+(safe?"PASS missing footstep data safely initialized":"FAILED stairs missing sound safety"));
        }
    }
    if(!IsClientReplica()) {
        const auto feet=gpBase->mpPlayer2->GetCharacterBody()->GetFeetPosition();
        mStairsApex=std::max(mStairsApex,feet.y-mStairsOrigin.y);
        if(!mStairsHostPassed && mStairsApex>1 && feet.z>mStairsOrigin.z-.15f && std::fabs(feet.y-mStairsOrigin.y)<.06f) {
            mStairsHostPassed=true; NetLog("[NET/TEST] PASS host authoritative stair ascent and descent apex="+std::to_string(mStairsApex));
        }
        return;
    }
    auto* player=gpBase->mpPlayer; auto* body=player->GetCharacterBody();
    const auto feet=body->GetFeetPosition();
    online::PlayerInput input; input.yaw=0;
    const float yaw=player->GetCamera()->GetYaw();
    const float add=std::atan2(std::sin(-yaw),std::cos(-yaw));
    player->AddYaw(add); input.lookYaw=int16_t(add*10000);
    if(mStairsStage==0 && (feet-mStairsOrigin).Length()<.05f) {
        mStairsStage=1; mStairsAge=0; mStairsPrevious=feet;
    }
    if(mStairsStage>0 && mStairsStage<5) {
        mStairsAge+=dt;
        mStairsApex=std::max(mStairsApex,feet.y-mStairsOrigin.y);
        mStairsMaxStep=std::max(mStairsMaxStep,std::fabs(feet.y-mStairsPrevious.y));
        const float reverse=mStairsStage==1?feet.z-mStairsPrevious.z:mStairsStage==3?mStairsPrevious.z-feet.z:0;
        mStairsMaxReverse=std::max(mStairsMaxReverse,reverse);
        mStairsPrevious=feet;
        if(mStairsStage==1) {
            input.forward=1000;
            if(feet.z<mStairsOrigin.z-5.6f) {mStairsStage=2;mStairsAge=0;input.forward=0;}
        } else if(mStairsStage==2 && mStairsAge>1) {mStairsStage=3;mStairsAge=0;}
        else if(mStairsStage==3) {
            input.forward=-1000;
            if(feet.z>mStairsOrigin.z) {mStairsStage=4;mStairsAge=0;input.forward=0;}
        } else if(mStairsStage==4 && mStairsAge>1) {
            const bool ok=mStairsApex>1 && std::fabs(feet.y-mStairsOrigin.y)<.06f && mStairsMaxStep<.12f && mStairsMaxReverse<.12f;
            NetLog(std::string("[NET/TEST] ")+(ok?"PASS client stair ascent and descent":"FAILED stairs client motion")+
                " apex="+std::to_string(mStairsApex)+" final_height="+std::to_string(feet.y-mStairsOrigin.y)+" max_vertical_step="+std::to_string(mStairsMaxStep)+" max_reverse="+std::to_string(mStairsMaxReverse));
            mStairsStage=5;
        }
        if(mStairsAge>15) {NetLog("[NET/TEST] FAILED stairs traversal timeout stage="+std::to_string(mStairsStage));mStairsStage=5;input.forward=0;}
    }
    SendLocalInput(input,dt);
}
void cLuxNetwork::UpdateEffectsTest(cLuxMap* map) {
    if(!map || !mSession.Connected() || !mSession.IsWorldApplied(mEpoch) || !gpBase->mpPlayer2) return;
    const auto now=std::chrono::steady_clock::now();
    auto* effects=gpBase->mpEffectHandler;
    auto* vision=effects->GetEmotionFlash();
    auto* local=gpBase->mpPlayer;
    if(!mEffectsTestBaseFov) mEffectsTestBaseFov=local->GetBaseFOV();
    if(IsClientReplica()) {
        if(effects->GetSanityGainFlash()->VisibleForPlayer(local)) mEffectsTestProgressSeen=true;
        const float age=std::chrono::duration<float>(now-mEffectsTestAt).count();
        if(mEffectsTestStarts==1 && mEffectsTestStage==0 && age>3) {
            vision->DoActionForPlayer(local,eLuxPlayerAction_Interact,true);
            mEffectsTestStage=1;
            NetLog("[NET/TEST] Client continued original emotion stone while inactive");
        }
        if(mEffectsTestEnds>=3 && !vision->IsActive() && local->IsActive() && mEffectsTestStage==1) {
            mEffectsTestFeet=local->GetCharacterBody()->GetFeetPosition();
            mEffectsTestAt=now; mEffectsTestStage=2;
        }
        online::PlayerInput input;
        if(mEffectsTestStage==2) {
            const float moving=std::chrono::duration<float>(now-mEffectsTestAt).count();
            input.strafe=moving<2?500:0;
            // Native FOV restoration is exponential and can outlast the white
            // overlay. Verify it after settling, while idle input stays alive.
            if(moving>3 && std::fabs(local->GetCamera()->GetFOV()-mEffectsTestBaseFov)<.01f) {
                const float distance=(local->GetCharacterBody()->GetFeetPosition()-mEffectsTestFeet).Length();
                const bool ok=mEffectsTestProgressSeen && local->IsActive() && !effects->GetPlayerIsPaused() && distance>.05f;
                NetLog(std::string("[NET/TEST] ")+(ok?"PASS objective flash and emotion page recovery":"FAILED effects client movement or flash")+" displacement="+std::to_string(distance));
                mEffectsTestStage=3;
            }
        }
        if(mEffectsTestStage==3 && mEffectsTestJumpReady &&
            (local->GetCharacterBody()->GetFeetPosition()-mEffectsTestJumpSpawn).Length()<.01f) {
            mEffectsTestJumpBase=local->GetCharacterBody()->GetFeetPosition().y;
            mEffectsTestAt=now; mEffectsTestStage=4;
        }
        if(mEffectsTestStage==4) {
            const float age=std::chrono::duration<float>(now-mEffectsTestAt).count();
            input.strafe=age<2?500:0; input.buttons=age>1 && age<1.2f?1:0;
            const float height=local->GetCharacterBody()->GetFeetPosition().y-mEffectsTestJumpBase;
            static float previousHeight=0,maxStep=0;
            const float step=std::fabs(height-previousHeight);
            if(step>.15f) NetLog("[NET/TEST] Vertical jump discontinuity age="+std::to_string(age)+" step="+std::to_string(step)+" pending="+std::to_string(mVerticalPrediction.Pending()));
            maxStep=std::max(maxStep,step); previousHeight=height;
            mEffectsTestJumpApex=std::max(mEffectsTestJumpApex,height);
            if(mVerticalPrediction.Pending() && !mSnapshots.empty() &&
                local->GetCharacterBody()->GetFeetPosition().y>mSnapshots.back().state.players[1].y/1000.0f+.01f) {
                if(!mEffectsTestJumpImmediate) NetLog("[NET/TEST] Client jump visible before host acknowledgement");
                mEffectsTestJumpImmediate=true;
            }
            if(age>5) {
                const bool grounded=!mSnapshots.empty() && mSnapshots.back().state.players[1].grounded;
                const bool ok=mEffectsTestJumpImmediate && mEffectsTestJumpApex>.2f && grounded && std::fabs(height)<.08f && maxStep<.2f;
                NetLog(std::string("[NET/TEST] ")+(ok?"PASS client jump prediction and landing":"FAILED effects client jump")+
                    " apex="+std::to_string(mEffectsTestJumpApex)+" landed_delta="+std::to_string(height)+" max_step="+std::to_string(maxStep));
                mEffectsTestStage=5;
            }
        }
        if(mEffectsTestStage==5 && !mSnapshots.empty() && !mSnapshots.back().state.players[1].jumpSpeed && mSnapshots.back().state.players[1].grounded) {
            mEffectsTestJumpBase=local->GetCharacterBody()->GetFeetPosition().y;
            mEffectsTestJumpApex=0; mEffectsTestAt=now; mEffectsTestStage=6;
        }
        if(mEffectsTestStage==6) {
            const float age=std::chrono::duration<float>(now-mEffectsTestAt).count();
            input.buttons=age<.2f?1:0;
            mEffectsTestJumpApex=std::max(mEffectsTestJumpApex,std::fabs(local->GetCharacterBody()->GetFeetPosition().y-mEffectsTestJumpBase));
            if(age>2) {
                const bool ok=!mVerticalPrediction.Pending() && mEffectsTestJumpApex<.05f;
                NetLog(ok?"[NET/TEST] PASS client respects scripted jump disable":"[NET/TEST] FAILED effects disabled client jump");
                mEffectsTestStage=7;
            }
        }
        // Native idle input continues during and after reading. Keep the same
        // heartbeat here so the fixture never triggers the missing-input lease.
        input.yaw=int16_t(local->GetCamera()->GetYaw()*1000); input.pitch=int16_t(local->GetCamera()->GetPitch()*1000);
        SendLocalInput(input,1.0f/60);
        return;
    }
    auto* partner=gpBase->mpPlayer2;
    const float age=std::chrono::duration<float>(now-mEffectsTestAt).count();
    auto stone=[&](cLuxPlayer* actor) {
        auto* entity=map->GetEntityByName("emotion_stone_1",eLuxEntityType_Prop,eLuxPropType_EmotionStone);
        if(!entity) { NetLog("[NET/TEST] FAILED effects original emotion stone missing"); mEffectsTestStage=99; return; }
        cLuxPlayer* previous=cLuxPlayer::CoopBeginActAs(actor);
        static_cast<cLuxProp_EmotionStone*>(entity)->OnInteract(NULL,cVector3f(0));
        cLuxPlayer::CoopEndActAs(previous);
        mEffectsTestAt=now;
    };
    if(mEffectsTestStage==0 && std::chrono::duration<float>(now-mSmokeStart).count()>5) {
        local->ApplyNetworkVitals(local->GetHealth(),40,local->GetLampOil());
        partner->ApplyNetworkVitals(partner->GetHealth(),40,partner->GetLampOil());
        map->RunScript("GiveSanityBoost();");
        NetLog("[NET/TEST] Original script objective sanity reward requested for both players");
        mEffectsTestAt=now; mEffectsTestStage=1;
    } else if(mEffectsTestStage==1 && age>3) {
        stone(partner); if(mEffectsTestStage!=99) mEffectsTestStage=2;
    } else if(mEffectsTestStage==2 && age>9 && !vision->IsActive()) {
        if(!mEffectsTestRemoteSkip || !partner->IsActive()) { NetLog("[NET/TEST] FAILED effects client advance did not unlock authoritative partner"); mEffectsTestStage=99; return; }
        NetLog("[NET/TEST] Remote emotion reader restored authoritative controls");
        stone(local); if(mEffectsTestStage!=99) mEffectsTestStage=3;
    } else if(mEffectsTestStage==3 && age>3) {
        vision->DoActionForPlayer(local,eLuxPlayerAction_Interact,true);
        mEffectsTestStage=4;
    } else if(mEffectsTestStage==4 && age>9 && !vision->IsActive()) {
        // A short original translation exercises natural expiry without a skip.
        vision->Start("Journal","Notes","");
        mEffectsTestAt=now; mEffectsTestStage=5;
    } else if(mEffectsTestStage==5 && age>22 && !vision->IsActive()) {
        const bool ok=local->IsActive() && partner->IsActive() && !effects->GetPlayerIsPaused() && std::fabs(local->GetCamera()->GetFOV()-mEffectsTestBaseFov)<.01f;
        NetLog(std::string("[NET/TEST] ")+(ok?"PASS objective flash and emotion page recovery":"FAILED effects host controls")+
            " original stone client/host advance and natural expiry host_active="+std::to_string(local->IsActive())+
            " partner_active="+std::to_string(partner->IsActive())+" paused="+std::to_string(effects->GetPlayerIsPaused())+
            " fov_delta="+std::to_string(std::fabs(local->GetCamera()->GetFOV()-mEffectsTestBaseFov)));
        mEffectsTestHostFeet=local->GetCharacterBody()->GetFeetPosition();
        partner->GetCharacterBody()->SetFeetPosition(mEffectsTestHostFeet);
        partner->GetCharacterBody()->SetForceVelocity(cVector3f(0));
        online::Presentation ready; ready.kind=online::PresentationKind::Message; ready.target=2; ready.text[0]="AMNESIA_QA_JUMP_READY";
        ready.values[0]=int32_t(mEffectsTestHostFeet.x*1000); ready.values[1]=int32_t(mEffectsTestHostFeet.y*1000); ready.values[2]=int32_t(mEffectsTestHostFeet.z*1000);
        QueuePresentation(ready);
        mEffectsTestJumpBase=mEffectsTestHostFeet.y; mEffectsTestAt=now; mEffectsTestStage=6;
        NetLog("[NET/TEST] Overlapped players for online collision and jump regression");
    } else if(mEffectsTestStage==6) {
        mEffectsTestJumpApex=std::max(mEffectsTestJumpApex,partner->GetCharacterBody()->GetFeetPosition().y-mEffectsTestJumpBase);
        if(age>10) {
            auto* a=local->GetCharacterBody(); auto* b=partner->GetCharacterBody();
            const float hostMoved=(a->GetFeetPosition()-mEffectsTestHostFeet).Length();
            const bool ok=a->GetIgnoreBody()==b->GetCurrentBody() && b->GetIgnoreBody()==a->GetCurrentBody() &&
                hostMoved<.08f && mEffectsTestJumpApex>.2f && b->IsOnGround();
            NetLog(std::string("[NET/TEST] ")+(ok?"PASS authoritative jump and players pass through":"FAILED effects host jump or player collision")+
                " apex="+std::to_string(mEffectsTestJumpApex)+" host_displacement="+std::to_string(hostMoved));
            partner->SetJumpDisabled(true);
            mEffectsTestJumpBase=b->GetFeetPosition().y; mEffectsTestAt=now; mEffectsTestStage=7;
        }
    } else if(mEffectsTestStage==7 && age>4) {
        const bool ok=partner->GetJumpDisabled() && std::fabs(partner->GetCharacterBody()->GetFeetPosition().y-mEffectsTestJumpBase)<.05f;
        NetLog(ok?"[NET/TEST] PASS host respects scripted jump disable":"[NET/TEST] FAILED effects disabled host jump");
        partner->SetJumpDisabled(false); mEffectsTestStage=8;
    }
}
#include "LuxNetwork_Serialize.inl"
#include "LuxNetwork_Checkpoint.inl"
