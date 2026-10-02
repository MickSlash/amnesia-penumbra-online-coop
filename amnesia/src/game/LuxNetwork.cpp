// SPDX-License-Identifier: GPL-3.0-or-later
#include "LuxNetwork.h"
#include "LuxPlayer.h"
#include "LuxMapHandler.h"
#include "LuxMap.h"
#include "impl/ImGuiConsole.h"
#include <cstdlib>
#include <cmath>
namespace {
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
            NetLog(std::string("[NET] role=") + role + " connected=" + (mSession.Connected() ? "yes" : "no") + " protocol=1; gameplay replication pending");
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
void cLuxNetwork::Update(float) {
    gpBase->mpEngine->SetWaitIfAppOutOfFocus(
        mSession.GetRole() == online::Role::Offline && !mSmokeSeconds ? mOriginalFocusWait : false);
    mSession.Poll();
    cLuxMap* map = gpBase->mpMapHandler->GetCurrentMap();
    if (mRemotePlayer2 && mRemotePlayer2 != gpBase->mpPlayer2) mRemotePlayer2 = NULL;
    if (mSession.GetRole() == online::Role::Host && mSession.Connected() && map &&
        gpBase->mpPlayer->GetCharacterBody()) {
        if (!gpBase->mpPlayer2 && !gpBase->mpMapHandler->GetCoopMode()) {
            gpBase->mpMapHandler->CreatePlayer2(map);
            mRemotePlayer2 = gpBase->mpPlayer2;
            if (!PlaceRemotePlayer2()) {
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
            NetLog("[NET/PLAYER] Created authoritative Player 2; full-screen host viewport retained");
        } else if (OwnsRemotePlayer2() && !mRemotePlayer2->GetCharacterBody()) {
            // MapHandler destroys P2 world entities on map leave. Reattach only
            // after P1 has entered and acquired a body in the new world.
            mRemotePlayer2->CreateWorldEntities(map);
            mRemotePlayer2->OnMapEnter(map);
            mRemotePlayer2->ReenterCurrentMoveState();
            gpBase->mpMapHandler->PlacePlayer2NearPlayer1();
            if (!PlaceRemotePlayer2()) {
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
}
void cLuxNetwork::SendLocalInput(online::PlayerInput input, float timeStep) {
    ++mInputTick;
    mInputAccumulator += timeStep;
    // 30 Hz divides the engine's default 60 Hz logic rate; render FPS is irrelevant.
    if (mInputAccumulator < 1.0f / 30.0f) return;
    mInputAccumulator = std::fmod(mInputAccumulator, 1.0f / 30.0f);
    input.sequence = ++mInputSequence; input.tick = mInputTick;
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
    const bool hasInput = mSession.ReadInput(input);
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
        player->AddYaw(std::atan2(std::sin(delta), std::cos(delta)));
        player->AddPitch(input.pitch / 1000.0f - player->GetCamera()->GetPitch());
    }
    if ((buttons ^ mPreviousButtons) & 1) player->Jump((buttons & 1) != 0);
    if ((buttons ^ mPreviousButtons) & 2) player->Crouch((buttons & 2) != 0);
    player->Run((buttons & 4) != 0);
    mPreviousButtons = buttons;
    return true;
}
