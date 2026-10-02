// SPDX-License-Identifier: GPL-3.0-or-later
#include "LuxNetwork.h"
#include "LuxPlayer.h"
#include "LuxMapHandler.h"
#include "LuxMap.h"
#include "LuxInputHandler.h"
#include "LuxPlayerState.h"
#include "LuxPlayerHelpers.h"
#include "LuxPlayerAvatar.h"
#include "LuxProp.h"
#include "LuxProp_Lamp.h"
#include "LuxProp_SwingDoor.h"
#include "LuxEnemy.h"
#include "LuxInventory.h"
#include "LuxSaveHandler.h"
#include "LuxEffectHandler.h"
#include "LuxJournal.h"
#include "LuxMessageHandler.h"
#include <cstdio>
#include "impl/ImGuiConsole.h"
#include <cstdlib>
#include <cmath>
#include <algorithm>
namespace {
bool TestFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string(value) == "1";
}
bool Port(const std::string& text, uint16_t& value) {
    if (text.empty() || text.size() > 5) return false;
    unsigned number = 0;
    for (char c : text) { if (c < '0' || c > '9') return false; number = number * 10 + (c - '0'); }
    if (!number || number > 65535) return false;
    value = uint16_t(number); return true;
}
void NetLog(const std::string& line) {
    Log("%s\n", line.c_str());
    cImGuiConsole::AddLog("%s", line.c_str());
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
}
cLuxNetwork::cLuxNetwork() : iLuxUpdateable("LuxNetwork"), mSession(NetLog) {}
cLuxNetwork::~cLuxNetwork() {
    for (const char* name : {"net_host", "net_join", "net_disconnect", "net_status", "net_ping"})
        cImGuiConsole::UnregisterCommand(name);
}
void cLuxNetwork::OnStart() {
    mOriginalFocusWait = gpBase->mpEngine->GetWaitIfAppOutOfFocus();
    const char* smoke = std::getenv("AMNESIA_NET_SMOKE_SECONDS");
    if (smoke && Port(smoke, mSmokeSeconds) && mSmokeSeconds <= 60)
        mSmokeStart = std::chrono::steady_clock::now();
    else mSmokeSeconds = 0;
    mTestMovement = mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_MOVEMENT");
    cImGuiConsole::RegisterCommand("net_host", "net_host [port] - listen (connectivity prototype)",
        [this](const cImGuiConsole::tArgs& args) {
            uint16_t port = online::DefaultPort;
            if (args.size() > 1 || (!args.empty() && !Port(args[0], port))) { NetLog("[NET] Usage: net_host [1..65535]"); return; }
            mSession.StartHost(port);
        });
    cImGuiConsole::RegisterCommand("net_join", "net_join IP [port] - connect (connectivity prototype)",
        [this](const cImGuiConsole::tArgs& args) {
            uint16_t port = online::DefaultPort;
            if (args.empty() || args.size() > 2 || (args.size() == 2 && !Port(args[1], port))) { NetLog("[NET] Usage: net_join IP [1..65535]"); return; }
            mSession.StartClient(args[0], port);
        });
    cImGuiConsole::RegisterCommand("net_disconnect", "net_disconnect - close session",
        [this](const cImGuiConsole::tArgs&) { mSession.Stop(); NetLog("[NET/CONNECTION] Session closed"); });
    cImGuiConsole::RegisterCommand("net_status", "net_status - role and handshake status",
        [this](const cImGuiConsole::tArgs&) {
            const char* role = mSession.GetRole() == online::Role::Host ? "host" : mSession.GetRole() == online::Role::Client ? "client" : "offline";
            NetLog(std::string("[NET] role=") + role + " connected=" + (mSession.Connected() ? "yes" : "no") + " protocol=2; epoch=" + std::to_string(mEpoch));
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
    if (std::string(role) == "host") mSession.StartHost(port);
    else if (std::string(role) == "client") {
        const char* address = std::getenv("AMNESIA_NET_ADDRESS");
        mSession.StartClient(address ? address : "127.0.0.1", port);
    } else NetLog("[NET] AMNESIA_NET_ROLE must be host or client");
    if (mSession.GetRole() != online::Role::Offline || mSmokeSeconds)
        gpBase->mpEngine->SetWaitIfAppOutOfFocus(false);
}
void cLuxNetwork::Update(float timeStep) {
    gpBase->mpEngine->SetWaitIfAppOutOfFocus(
        mSession.GetRole() == online::Role::Offline && !mSmokeSeconds ? mOriginalFocusWait : false);
    mSession.Poll();
    if(IsClientReplica()) {
        mWasClient = true;
        if(mSession.Connected()) mHadClientConnection = true;
        else if(mHadClientConnection) mSession.Stop();
    }
    if(mWasClient && !IsClientReplica()) {
        mWasClient = mHadClientConnection = false;
        gpBase->mpEngine->GetUpdater()->BroadcastMessageToAll(eUpdateableMessage_Reset);
        gpBase->mpInputHandler->ChangeState(eLuxInputState_MainMenu);
        gpBase->mpEngine->GetUpdater()->SetContainer("MainMenu");
        NetLog("[NET/CONNECTION] Client session ended; replica world unloaded");
    }
    UpdateReplication(timeStep);
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
    } else if (OwnsRemotePlayer2()) {
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
        gpBase->mpEngine->Exit();
    }
}
bool cLuxNetwork::OwnsRemotePlayer2() const {
    return mRemotePlayer2 && mRemotePlayer2 == gpBase->mpPlayer2;
}
bool cLuxNetwork::PlaceRemotePlayer2() {
    iCharacterBody* body = mRemotePlayer2->GetCharacterBody();
    iCharacterBody* hostBody = gpBase->mpPlayer->GetCharacterBody();
    iPhysicsWorld* world = gpBase->mpMapHandler->GetCurrentMap()->GetPhysicsWorld();
    // Local co-op's unconditional one-metre offset can put P2 inside a wall.
    // Test the actual character shape against the host's world before spawning.
    const float radii[] = { 1.0f, 0.8f, 1.3f };
    for (float radius : radii) {
        for (int direction = 0; direction < 8; ++direction) {
            float angle = hostBody->GetYaw() + direction * 0.785398163f;
            cVector3f feet = hostBody->GetFeetPosition() + cVector3f(std::cos(angle) * radius, 0.03f, -std::sin(angle) * radius);
            cVector3f centre = feet + cVector3f(0, body->GetCurrentShape()->GetSize().y / 2, 0);
            if (!world->CheckShapeWorldCollision(NULL, body->GetCurrentShape(), cMath::MatrixTranslate(centre), NULL, false, true, NULL, false)) {
                body->SetFeetPosition(feet);
                body->Update(0.001f);
                return true;
            }
        }
    }
    return false;
}
void cLuxNetwork::Reset() {
    mRemotePlayer2 = NULL; mPreviousButtons = 0;
    mInputAccumulator = 0;
    mSnapshots.clear(); mReceivedSnapshot = false; mReceivedSequence = 0;
    mEntityBytes.clear(); mEntityStates.clear(); mReplicaIDs.clear(); mLoggedWorldReplica = false;
    mBaselineApplied = false; mBaselineSent = false; mClientMoved = false;
    mAppliedInventory = false; mInventoryBytes.clear();
    mPendingPresentation.clear(); mLightBytes.clear(); mLoggedLighting = false;
}
void cLuxNetwork::SendLocalInput(online::PlayerInput input, float timeStep) {
    if (IsClientReplica() && (!mEpoch || !mReceivedSnapshot || !mBaselineApplied)) return;
    ++mInputTick;
    mInputAccumulator += timeStep;
    // 30 Hz divides the engine's default 60 Hz logic rate; render FPS is irrelevant.
    if (mInputAccumulator < 1.0f / 30.0f) return;
    mInputAccumulator = std::fmod(mInputAccumulator, 1.0f / 30.0f);
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
                mTestActionTimer += 1.0f / 30.0f;
                if(mTestActionTimer > 0.5f) { SendLocalAction(online::PlayerAction::Interact, true); SendLocalAction(online::PlayerAction::Interact, false); mTestActionTimer = 0; }
            }
        }
    }
    mSession.SendInput(input);
}
void cLuxNetwork::OnExit() {
    mSession.Stop();
    gpBase->mpEngine->SetWaitIfAppOutOfFocus(mOriginalFocusWait);
}
bool cLuxNetwork::ApplyRemotePlayerInput() {
    if (mSession.GetRole() != online::Role::Host) return false;
    // Host networking owns the P2 input slot, including before/after connection.
    cLuxPlayer* player = gpBase->mpPlayer2;
    if (!player || !player->GetCharacterBody()) { mPreviousButtons = 0; return true; }
    online::PlayerInput input;
    const bool hasInput = !WaitingForMap() && mSession.ReadInput(input);
    const bool active = hasInput && player->IsActive() && !player->IsDead();
    if (mSmokeSeconds && hasInput && !mLoggedRemoteInput) {
        mLoggedRemoteInput = true;
        NetLog(std::string("[NET/TEST] Applying received input; player active=") + (active ? "yes" : "no"));
    }
    uint8_t buttons = active ? input.buttons : 0;
    if (active) {
        cVector3f displacement = player->GetCharacterBody()->GetFeetPosition() - mRemoteSpawnFeet;
        if (mSmokeSeconds && OwnsRemotePlayer2() && !mObservedRemoteMovement &&
            (input.forward || input.strafe) && displacement.x * displacement.x + displacement.z * displacement.z > 0.01f) {
            mObservedRemoteMovement = true;
            NetLog("[NET/TEST] Remote Player 2 moved under network input");
        }
        player->Move(eCharDir_Forward, input.forward / 1000.0f);
        player->Move(eCharDir_Right, input.strafe / 1000.0f);
        float delta = input.yaw / 1000.0f - player->GetCamera()->GetYaw();
        // AddYaw/AddPitch expect raw input and internally multiply it by two.
        // The wire contains the resulting camera angle, so halve the correction.
        player->AddYaw(std::atan2(std::sin(delta), std::cos(delta)) * 0.5f);
        player->AddPitch((input.pitch / 1000.0f - player->GetCamera()->GetPitch()) * 0.5f);
    }
    if ((buttons ^ mPreviousButtons) & 1) player->Jump((buttons & 1) != 0);
    if ((buttons ^ mPreviousButtons) & 2) player->Crouch((buttons & 2) != 0);
    player->Run((buttons & 4) != 0);
    online::ActionRequest action;
    while(mSession.ReadAction(action)) {
        if(!player->IsActive() || player->IsDead() || WaitingForMap() || action.epoch != mEpoch) continue;
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
        eLuxPlayerAction type = action.action == online::PlayerAction::Interact ? eLuxPlayerAction_Interact :
            action.action == online::PlayerAction::Lantern ? eLuxPlayerAction_Lantern :
            action.action == online::PlayerAction::Ignite ? eLuxPlayerAction_Ignite : eLuxPlayerAction_Attack;
        player->DoAction(type, action.pressed);
        if(action.action == online::PlayerAction::Interact) mRemoteInteractHeld = action.pressed;
    }
    if(!active && mRemoteInteractHeld) { player->DoAction(eLuxPlayerAction_Interact, false); mRemoteInteractHeld = false; }
    mPreviousButtons = buttons;
    return true;
}
void cLuxNetwork::SendLocalAction(online::PlayerAction action, bool pressed) {
    if(!IsClientReplica() || !mReceivedSnapshot || !mEpoch || !mBaselineApplied) return;
    online::ActionRequest request; request.epoch = mEpoch; request.sequence = ++mActionSequence;
    request.action = action; request.pressed = pressed; mSession.SendAction(request);
}
void cLuxNetwork::SendInventoryAction(online::PlayerAction action, const std::string& item, const std::string& secondItem) {
    if(!IsClientReplica() || !mReceivedSnapshot || !mBaselineApplied) return;
    online::ActionRequest request; request.epoch = mEpoch; request.sequence = ++mActionSequence;
    request.action = action; request.pressed = true; request.item = item; request.secondItem = secondItem; mSession.SendAction(request);
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
bool cLuxNetwork::WaitingForMap() const {
    return mSession.GetRole() == online::Role::Host && mSession.Connected() &&
        mMapAnnounced && !mSession.IsMapReady(mEpoch);
}
void cLuxNetwork::LoadReplicaMap(const online::MapState& state) {
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
    const size_t slash = state.file.find_last_of('/');
    if(!gpBase->mpMapHandler->GetCurrentMap()) {
        gpBase->StartGame(state.file.substr(slash + 1), state.file.substr(0, slash + 1), "");
    } else {
        cLuxMap* old = gpBase->mpMapHandler->GetCurrentMap();
        gpBase->mpMapHandler->SetMapFolder(state.file.substr(0, slash + 1));
        cLuxMap* next = gpBase->mpMapHandler->LoadMap(state.file.substr(slash + 1), true);
        gpBase->mpMapHandler->DestroyMap(old, false);
        gpBase->mpMapHandler->SetCurrentMap(next, false, false, "");
        mReplicaIDs.clear(); mBaselineApplied = false; mAppliedInventory = false;
        mLoggedWorldReplica = false; mLoggedLighting = false;
    }
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if(!map || !gpBase->mpPlayer->GetCharacterBody()) {
        NetLog("[NET/MAP] Replica map load failed; closing session"); mSession.Stop(); return;
    }
    mEpoch = state.epoch;
    mSnapshots.clear(); mReceivedSnapshot = false;
    map->GetWorld()->SetPhysicsSimulationEnabled(false);
    mSession.SendMapReady(mEpoch);
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
    body->SetYaw(localView && (state.flags & 4) ? player->GetCamera()->GetYaw() : state.yaw / 1000.0f);
    if(!localView || !(state.flags & 4)) {
        player->GetCamera()->SetYaw(state.yaw / 1000.0f);
        player->GetCamera()->SetPitch(state.pitch / 1000.0f);
    }
    // Physics is disabled on replicas; position the camera without advancing a body.
    player->GetCamera()->SetPosition(body->GetFeetPosition() + cVector3f(0, state.eyeHeight / 1000.0f, 0));
    player->ApplyNetworkVitals(state.health / 100.0f, state.sanity / 100.0f, state.oil / 100.0f);
    player->SetTinderboxes(state.tinderboxes);
    player->SetActive((state.flags & 4) != 0);
    player->GetHelperLantern()->SetActive((state.flags & 2) != 0, false, false, false);
}
void cLuxNetwork::UpdateReplication(float timeStep) {
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    const auto now = std::chrono::steady_clock::now();
    if(!mSession.Connected()) {
        mMapAnnounced = false; mHostMapFile.clear();
        mSnapshots.clear(); mReceivedSnapshot = false;
        if(map) map->GetWorld()->SetPhysicsSimulationEnabled(!IsClientReplica());
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
        online::Presentation event;
        for(int count = 0; count < 128 && mSession.ReadPresentation(event); ++count)
            if(event.epoch == mEpoch && event.target != 1) ApplyPresentation(map, event);
        online::EntityState entity;
        for(int count = 0; count < 128 && mSession.ReadEntity(entity); ++count)
            if(entity.epoch == mEpoch) ApplyWorldEntity(map, entity);
        if(!mBaselineApplied && mSession.IsWorldReady(mEpoch)) {
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
        if(mSession.ReadInventory(inventory) && inventory.epoch == mEpoch && (!mAppliedInventory || inventory.revision != mAppliedInventoryRevision)) {
            gpBase->mpInventory->ApplyNetworkInventory(inventory);
            if(!mAppliedInventory) NetLog("[NET/INVENTORY] Shared host inventory applied");
            mAppliedInventory = true; mAppliedInventoryRevision = inventory.revision;
        }
        online::Snapshot snapshot;
        if(mSession.ReadSnapshot(snapshot) && (!mReceivedSnapshot || snapshot.sequence != mReceivedSequence)) {
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
            }
        }
        // The local viewport stays P1 internally; network ownership is explicit.
        ApplyReplica(gpBase->mpPlayer, interpolated.players[1], true);
        ApplyReplica(gpBase->mpPlayer2, interpolated.players[0], false);
        const bool dead = (interpolated.players[1].flags & 8) != 0;
        if(dead != mClientDead) {
            mClientDead = dead;
            if(dead) gpBase->mpEffectHandler->GetFade()->FadeOut(0.35f);
            else gpBase->mpEffectHandler->GetFade()->FadeIn(0.5f);
            if(mSmokeSeconds) NetLog(dead ? "[NET/TEST] Authoritative death applied on client" : "[NET/TEST] Authoritative checkpoint revival applied on client");
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
        if(!mSession.SendMap(state)) { NetLog("[NET/MAP] Cannot announce map; closing session"); mSession.Stop(); return; }
        mHostMapFile = map->GetFileName(); mMapAnnounced = true; mMapWaitStart = now;
        mEntityBytes.clear(); mEntityStates.clear();
        mBaselineSent = false;
        mInventoryBytes.clear();
        mLightBytes.clear();
        NetLog("[NET/MAP] Announced epoch=" + std::to_string(mEpoch) + " file=" + state.file);
    }
    map->GetWorld()->SetPhysicsSimulationEnabled(!WaitingForMap());
    if(WaitingForMap()) {
        if(now - mMapWaitStart > std::chrono::seconds(30)) {
            NetLog("[NET/MAP] Client map readiness timed out"); mSession.Stop();
        }
        return;
    }
    if(!gpBase->mpPlayer2 || !gpBase->mpPlayer2->GetCharacterBody()) return;
    if(mSmokeSeconds && TestFlag("AMNESIA_NET_TEST_ENEMY")) {
        if(!mTestEnemySpawned) {
            cLuxPlayer* player = gpBase->mpPlayer2;
            cVector3f position = player->GetCharacterBody()->GetFeetPosition() + player->GetCamera()->GetForward() * 3.0f;
            map->CreateEntity("online_test_grunt", "entities/enemy/servant_grunt/servant_grunt.ent", cMath::MatrixTranslate(position), cVector3f(1));
            iLuxEntity* entity = map->GetEntityByName("online_test_grunt");
            if(entity && entity->GetEntityType() == eLuxEntityType_Enemy) {
                iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
                enemy->SetActive(true); enemy->ChangeState(eLuxEnemyState_Wait); enemy->SetDisabled(true);
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
        if(!mTestSaveLoaded && elapsed > 8) {
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
    mSnapshotAccumulator += timeStep;
    if(mSnapshotAccumulator < 1.0f / 20.0f) return;
    mSnapshotAccumulator = std::fmod(mSnapshotAccumulator, 1.0f / 20.0f);
    online::Snapshot snapshot; snapshot.epoch = mEpoch; snapshot.sequence = ++mSnapshotSequence;
    online::PlayerInput input; if(mSession.ReadInput(input)) snapshot.acknowledgedInput = input.sequence;
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
        p.tinderboxes = uint16_t(std::max(0, std::min(65535, player->GetTinderboxes())));
        p.flags = (player->GetCharacterBody()->GetActiveSize() == 1 ? 1 : 0) |
            (player->GetHelperLantern()->IsActive() ? 2 : 0) | (player->IsActive() ? 4 : 0) | (player->IsDead() ? 8 : 0);
    }
    const bool snapshotSent = mSession.SendSnapshot(snapshot);
    const bool dead = (snapshot.players[1].flags & 8) != 0;
    if(mSmokeSeconds && snapshotSent && (!mTestHasLifeSnapshot || mTestLastSentDead != dead)) {
        mTestHasLifeSnapshot = true; mTestLastSentDead = dead;
        NetLog("[NET/TEST] Sent remote life snapshot dead=" + std::to_string(dead ? 1 : 0) + " sequence=" + std::to_string(snapshot.sequence));
    }
    ReplicateWorld(map);
    ReplicateLights(map);
    for(unsigned count = 0; count < 64 && !mPendingPresentation.empty(); ++count) {
        auto event = mPendingPresentation.front(); event.epoch = mEpoch;
        if(!mSession.SendPresentation(event)) break;
        mPendingPresentation.pop_front();
    }
    online::InventoryState inventory = gpBase->mpInventory->CaptureNetworkInventory(mEpoch);
    auto inventoryBytes = online::EncodeInventory(inventory);
    if(!inventoryBytes.empty() && inventoryBytes != mInventoryBytes) {
        inventory.revision = ++mInventoryRevision;
        if(mSession.SendInventory(inventory)) mInventoryBytes = std::move(inventoryBytes);
    }
}
void cLuxNetwork::ReplicateWorld(cLuxMap* map) {
    std::map<uint32_t, bool> present;
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
            }
            for(int i = 0; i < prop->GetBodyNum() && i < 32; ++i) state.bodies.push_back(PackTransform(prop->GetBody(i)->GetLocalMatrix()));
        } else if(state.type == eLuxEntityType_Enemy) {
            iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
            state.file = enemy->GetNetworkFile(); scale = enemy->GetNetworkScale();
            if(enemy->GetCharacterBody()) state.bodies.push_back(PackTransform(enemy->GetBody(0)->GetLocalMatrix()));
        }
        std::replace(state.file.begin(), state.file.end(), '\\', '/');
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
        if(sent < 64 && !bytes.empty() && (old == mEntityBytes.end() || old->second != bytes) && mSession.SendEntity(state)) {
            mEntityBytes[state.id] = std::move(bytes); mEntityStates[state.id] = state; ++sent;
        }
    }
    for(auto it = mEntityStates.begin(); it != mEntityStates.end();) {
        if(present.find(it->first) != present.end() || sent >= 64) { ++it; continue; }
        online::EntityState removed = it->second; removed.flags |= 4; removed.bodies.clear(); removed.animations.clear();
        if(!mSession.SendEntity(removed)) { ++it; continue; }
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
    if(!entity || entity->GetName() != state.name || uint8_t(entity->GetEntityType()) != state.type) return;
    mReplicaIDs[state.id] = entity->GetID();
    entity->SetActive((state.flags & 1) != 0); entity->SetInteractionDisabled((state.flags & 2) != 0);
    if(state.type == eLuxEntityType_Prop) {
        iLuxProp* prop = static_cast<iLuxProp*>(entity);
        if(uint16_t(prop->GetPropType()) != state.subtype) return;
        prop->SetEffectsActive((state.flags & 64) != 0, false);
        if(prop->GetPropType() == eLuxPropType_Lamp) static_cast<cLuxProp_Lamp*>(prop)->SetLit((state.flags & 8) != 0, false);
        if(prop->GetPropType() == eLuxPropType_SwingDoor) {
            auto* door = static_cast<cLuxProp_SwingDoor*>(prop);
            door->SetLocked((state.flags & 16) != 0, false); door->SetClosed((state.flags & 32) != 0, false);
        }
        for(size_t i = 0; i < state.bodies.size() && i < size_t(prop->GetBodyNum()); ++i)
            prop->GetBody(int(i))->SetMatrix(UnpackTransform(state.bodies[i]));
    } else if(state.type == eLuxEntityType_Enemy && !state.bodies.empty()) {
        iLuxEnemy* enemy = static_cast<iLuxEnemy*>(entity);
        if(enemy->GetCharacterBody()) enemy->GetCharacterBody()->SetPosition(UnpackTransform(state.bodies[0]).GetTranslation());
        if(mSmokeSeconds && state.name == "online_test_grunt") {
            cVector3f position = UnpackTransform(state.bodies[0]).GetTranslation();
            if(!mTestHasEnemyReplica) { mTestHasEnemyReplica = true; mTestReplicaEnemyInitialPosition = position; NetLog("[NET/TEST] Authoritative enemy created on client"); }
            cVector3f delta = position - mTestReplicaEnemyInitialPosition;
            if(!mTestReplicaEnemyMoved && delta.x*delta.x + delta.z*delta.z > 0.01f) { mTestReplicaEnemyMoved = true; NetLog("[NET/TEST] Authoritative enemy movement applied on client"); }
        }
    }
    cMeshEntity* mesh = entity->GetMeshEntity();
    if(mesh) {
        if(state.type == eLuxEntityType_Enemy || state.bodies.empty()) mesh->SetMatrix(UnpackTransform(state.transform));
        for(int i = 0; i < mesh->GetAnimationStateNum(); ++i) mesh->GetAnimationState(i)->SetActive(false);
        for(const auto& a : state.animations) if(a.index < mesh->GetAnimationStateNum()) {
            cAnimationState* animation = mesh->GetAnimationState(a.index);
            animation->SetActive(true); animation->SetWeight(a.weight / 10000.0f); animation->SetTimePosition(a.time / 1000.0f);
        }
    }
    if(!mLoggedWorldReplica) { mLoggedWorldReplica = true; NetLog("[NET/WORLD] Authoritative entity state applied"); }
}
void cLuxNetwork::ReplicateLights(cLuxMap* map) {
    unsigned sent = 0;
    cLightListIterator it = map->GetWorld()->GetLightIterator();
    while(it.HasNext()) {
        iLight* light = it.Next();
        if(light->GetName().find("PlayerLantern") == 0) continue; // ownership is swapped on the client
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
    switch(event.kind) {
    case online::PresentationKind::Light: {
        iLight* light = map->GetWorld()->GetLight(event.text[0]);
        if(!light || event.text[0].find("PlayerLantern") == 0) break;
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
