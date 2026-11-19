// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled by LuxBase.cpp. No SDK or Valve runtime is redistributed: the
// original game's steam_api64.dll is loaded and used through its flat C API.
#include "LuxSteam.h"
#include "network/MessageBridge.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace luxsteam {
namespace {
HMODULE runtime = nullptr;
bool initialized = false;
using InitFn = bool (__cdecl*)();
using VoidFn = void (__cdecl*)();
using InterfaceFn = void* (__cdecl*)();
using SubscribedFn = bool (__cdecl*)(void*);
using AppIdFn = unsigned int (__cdecl*)(void*);
VoidFn shutdown = nullptr, runCallbacks = nullptr;
bool Fail(std::string& error, const char* message) {
    error = message;
    Log("[STEAM] Initialization failed: %s\n", message);
    Shutdown();
    return false;
}

// --- Strutture dei callback (allineamento naturale x64, come nell'SDK) ---
struct CallbackMsg { int32_t user; int callback; uint8_t* param; int size; };
struct CallCompleted { uint64_t call; int callback; uint32_t size; };
struct LobbyCreated { int result; uint64_t lobby; };
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };
struct LobbyJoinRequested { uint64_t lobby; uint64_t user; };
struct PresenceJoinRequested { uint64_t user; char connect[256]; };
struct FriendGameInfo { uint64_t game; uint32_t ip; uint16_t port, queryPort; uint64_t lobby; };
// SteamNetworkingIdentity (136 byte): tipo, dimensione, poi lo SteamID.
struct Identity { int type; int size; uint64_t id; uint8_t rest[120]; };
static_assert(sizeof(Identity) == 136, "SteamNetworkingIdentity");
const int IdentitySteamID = 16;
const int CallLobbyCreated = 513, CallLobbyEnter = 504, CallCompletedId = 703,
    CallLobbyJoinRequested = 333, CallPresenceJoinRequested = 337, CallSessionRequest = 1251;
const uint32_t AppId = 57300;

using PipeFn = int32_t (__cdecl*)();
using RunFrameFn = void (__cdecl*)(int32_t);
using NextCallbackFn = bool (__cdecl*)(int32_t, CallbackMsg*);
using CallResultFn = bool (__cdecl*)(int32_t, uint64_t, void*, int, int, bool*);
using CreateLobbyFn = uint64_t (__cdecl*)(void*, int, int);
using LobbyFn = uint64_t (__cdecl*)(void*, uint64_t);
using LeaveLobbyFn = void (__cdecl*)(void*, uint64_t);
using InviteFn = bool (__cdecl*)(void*, uint64_t, uint64_t);
using SetLobbyDataFn = bool (__cdecl*)(void*, uint64_t, const char*, const char*);
using MemberCountFn = int (__cdecl*)(void*, uint64_t);
using MemberFn = uint64_t (__cdecl*)(void*, uint64_t, int);
using OverlayInviteFn = void (__cdecl*)(void*, uint64_t);
using PresenceFn = bool (__cdecl*)(void*, const char*, const char*);
using ClearPresenceFn = void (__cdecl*)(void*);
using FriendCountFn = int (__cdecl*)(void*, int);
using FriendByIndexFn = uint64_t (__cdecl*)(void*, int, int);
using NameFn = const char* (__cdecl*)(void*, uint64_t);
using StateFn = int (__cdecl*)(void*, uint64_t);
using GamePlayedFn = bool (__cdecl*)(void*, uint64_t, FriendGameInfo*);
using BoolFn = bool (__cdecl*)(void*);
using SteamIdFn = uint64_t (__cdecl*)(void*);
using SendFn = int (__cdecl*)(void*, const Identity*, const void*, uint32_t, int, int);
using ReceiveFn = int (__cdecl*)(void*, int, void**, int);
using AcceptFn = bool (__cdecl*)(void*, const Identity*);
using ReleaseFn = void (__cdecl*)(void*);
using LaunchLineFn = int (__cdecl*)(void*, char*, int);
using HasFriendFn = bool (__cdecl*)(void*, uint64_t, int);

struct Api {
    PipeFn pipe = nullptr; VoidFn dispatchInit = nullptr; RunFrameFn runFrame = nullptr; NextCallbackFn next = nullptr;
    RunFrameFn freeLast = nullptr; CallResultFn callResult = nullptr;
    void *matchmaking = nullptr, *friends = nullptr, *utils = nullptr, *user = nullptr, *messages = nullptr, *netUtils = nullptr, *apps = nullptr;
    CreateLobbyFn createLobby = nullptr; LobbyFn joinLobby = nullptr, lobbyOwner = nullptr; LeaveLobbyFn leaveLobby = nullptr;
    InviteFn invite = nullptr; SetLobbyDataFn setLobbyData = nullptr; MemberCountFn memberCount = nullptr; MemberFn member = nullptr;
    OverlayInviteFn overlayInvite = nullptr; PresenceFn presence = nullptr; ClearPresenceFn clearPresence = nullptr;
    FriendCountFn friendCount = nullptr; FriendByIndexFn friendByIndex = nullptr; NameFn friendName = nullptr; StateFn friendState = nullptr;
    GamePlayedFn gamePlayed = nullptr; BoolFn overlayEnabled = nullptr; SteamIdFn steamId = nullptr;
    SendFn send = nullptr; ReceiveFn receive = nullptr; AcceptFn accept = nullptr; ReleaseFn release = nullptr;
    VoidFn relay = nullptr; LaunchLineFn launchLine = nullptr; HasFriendFn hasFriend = nullptr;
    bool ready = false;
} api;
int32_t pipe = 0;

// Stato della lobby e coda degli eventi per il gioco.
uint64_t lobby = 0, localUser = 0;
bool fake = false;
std::deque<uint64_t> joinRequests;
struct Entered { uint64_t lobby, owner; bool ok; };
std::deque<Entered> entered;
std::unique_ptr<online::MessageTransport> fakeTransport;

template<class T> bool Load(T& target, const char* name) {
    target = reinterpret_cast<T>(GetProcAddress(runtime, name));
    return target != nullptr;
}
Identity MakeIdentity(uint64_t user) { Identity identity; std::memset(&identity, 0, sizeof(identity)); identity.type = IdentitySteamID; identity.size = 8; identity.id = user; return identity; }

class SteamTransport : public online::MessageTransport {
public:
    bool Send(uint64_t peer, const uint8_t* data, size_t size) override {
        if(!api.ready || !peer || size > 512 * 1024) return false;
        const Identity identity = MakeIdentity(peer);
        // Non affidabile e senza attesa (ENet gestisce l'affidabilità);
        // riapre da sé una sessione caduta.
        return api.send(api.messages, &identity, data, uint32_t(size), 1 | 32, 0) == 1;
    }
    bool Receive(uint64_t& peer, std::vector<uint8_t>& data) override {
        if(!api.ready) return false;
        void* message = nullptr;
        if(api.receive(api.messages, 0, &message, 1) != 1 || !message) return false;
        const uint8_t* bytes = static_cast<const uint8_t*>(message);
        // SteamNetworkingMessage_t: m_pData (0), m_cbSize (8), m_identityPeer (16).
        const void* payload = *reinterpret_cast<void* const*>(bytes);
        const int size = *reinterpret_cast<const int*>(bytes + 8);
        const Identity* identity = reinterpret_cast<const Identity*>(bytes + 16);
        peer = identity->type == IdentitySteamID ? identity->id : 0;
        data.assign(static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + (size > 0 ? size : 0));
        api.release(message);
        return true;
    }
} steamTransport;

bool LoadApi() {
    Api a;
    InterfaceFn matchmaking = nullptr, friends = nullptr, utils = nullptr, user = nullptr, messages = nullptr, netUtils = nullptr, apps = nullptr;
    bool ok = Load(a.pipe,"SteamAPI_GetHSteamPipe") && Load(a.dispatchInit,"SteamAPI_ManualDispatch_Init") &&
        Load(a.runFrame,"SteamAPI_ManualDispatch_RunFrame") && Load(a.next,"SteamAPI_ManualDispatch_GetNextCallback") &&
        Load(a.freeLast,"SteamAPI_ManualDispatch_FreeLastCallback") && Load(a.callResult,"SteamAPI_ManualDispatch_GetAPICallResult") &&
        Load(matchmaking,"SteamAPI_SteamMatchmaking_v009") && Load(friends,"SteamAPI_SteamFriends_v017") &&
        Load(utils,"SteamAPI_SteamUtils_v010") && Load(user,"SteamAPI_SteamUser_v023") &&
        Load(messages,"SteamAPI_SteamNetworkingMessages_SteamAPI_v002") && Load(netUtils,"SteamAPI_SteamNetworkingUtils_SteamAPI_v004") &&
        Load(apps,"SteamAPI_SteamApps_v008") &&
        Load(a.createLobby,"SteamAPI_ISteamMatchmaking_CreateLobby") && Load(a.joinLobby,"SteamAPI_ISteamMatchmaking_JoinLobby") &&
        Load(a.lobbyOwner,"SteamAPI_ISteamMatchmaking_GetLobbyOwner") && Load(a.leaveLobby,"SteamAPI_ISteamMatchmaking_LeaveLobby") &&
        Load(a.invite,"SteamAPI_ISteamMatchmaking_InviteUserToLobby") && Load(a.setLobbyData,"SteamAPI_ISteamMatchmaking_SetLobbyData") &&
        Load(a.memberCount,"SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") && Load(a.member,"SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex") &&
        Load(a.overlayInvite,"SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog") && Load(a.presence,"SteamAPI_ISteamFriends_SetRichPresence") &&
        Load(a.clearPresence,"SteamAPI_ISteamFriends_ClearRichPresence") && Load(a.friendCount,"SteamAPI_ISteamFriends_GetFriendCount") &&
        Load(a.friendByIndex,"SteamAPI_ISteamFriends_GetFriendByIndex") && Load(a.friendName,"SteamAPI_ISteamFriends_GetFriendPersonaName") &&
        Load(a.friendState,"SteamAPI_ISteamFriends_GetFriendPersonaState") && Load(a.gamePlayed,"SteamAPI_ISteamFriends_GetFriendGamePlayed") &&
        Load(a.overlayEnabled,"SteamAPI_ISteamUtils_IsOverlayEnabled") && Load(a.steamId,"SteamAPI_ISteamUser_GetSteamID") &&
        Load(a.send,"SteamAPI_ISteamNetworkingMessages_SendMessageToUser") && Load(a.receive,"SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel") &&
        Load(a.accept,"SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser") && Load(a.release,"SteamAPI_SteamNetworkingMessage_t_Release") &&
        Load(a.launchLine,"SteamAPI_ISteamApps_GetLaunchCommandLine") && Load(a.hasFriend,"SteamAPI_ISteamFriends_HasFriend");
    using RelayFn = void (__cdecl*)(void*);
    RelayFn relay = nullptr;
    ok = ok && Load(relay,"SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    if(!ok) return false;
    a.matchmaking = matchmaking(); a.friends = friends(); a.utils = utils(); a.user = user();
    a.messages = messages(); a.netUtils = netUtils(); a.apps = apps();
    if(!a.matchmaking || !a.friends || !a.utils || !a.user || !a.messages || !a.netUtils || !a.apps) return false;
    a.dispatchInit();
    relay(a.netUtils); // prepara subito i relay: il primo collegamento è più rapido
    a.ready = true; api = a; pipe = api.pipe(); localUser = api.steamId(api.user);
    return true;
}
// "+connect_lobby <id>" (gioco avviato da un invito) o lo stesso nella stringa
// di "Unisciti" della presenza.
uint64_t LobbyFromConnect(const std::string& text) {
    const auto at = text.find("+connect_lobby");
    if(at == std::string::npos) return 0;
    return std::strtoull(text.c_str() + at + 14, nullptr, 10);
}
bool LobbyMember(uint64_t user) {
    if(!lobby || !user) return false;
    const int count = api.memberCount(api.matchmaking, lobby);
    for(int i = 0; i < count; ++i) if(api.member(api.matchmaking, lobby, i) == user) return true;
    return false;
}
void SetPresence() {
    if(!api.ready) return;
    if(lobby) {
        const std::string connect = "+connect_lobby " + std::to_string(lobby);
        api.presence(api.friends, "connect", connect.c_str());
        api.presence(api.friends, "status", "Online co-op");
    } else api.clearPresence(api.friends);
}
void Dispatch() {
    api.runFrame(pipe);
    CallbackMsg message;
    while(api.next(pipe, &message)) {
        if(message.callback == CallCompletedId && message.param) {
            const auto* completed = reinterpret_cast<const CallCompleted*>(message.param);
            if(completed->callback == CallLobbyCreated) {
                LobbyCreated created = {}; bool failed = true;
                if(api.callResult(pipe, completed->call, &created, sizeof(created), CallLobbyCreated, &failed) && !failed && created.result == 1) {
                    lobby = created.lobby;
                    api.setLobbyData(api.matchmaking, lobby, "amnesia_online_coop", "18");
                    SetPresence();
                    Log("[STEAM] Friends-only lobby created\n");
                } else {
                    Log("[STEAM] Lobby creation failed (result %d)\n", created.result);
                    entered.push_back({0, 0, false});
                }
            }
        } else if(message.callback == CallLobbyEnter && message.param) {
            const auto* enter = reinterpret_cast<const LobbyEnter*>(message.param);
            const bool ok = enter->response == 1;
            if(ok) { lobby = enter->lobby; SetPresence(); }
            entered.push_back({enter->lobby, ok ? api.lobbyOwner(api.matchmaking, enter->lobby) : 0, ok});
            Log("[STEAM] Lobby entered (response %u)\n", enter->response);
        } else if(message.callback == CallLobbyJoinRequested && message.param) {
            joinRequests.push_back(reinterpret_cast<const LobbyJoinRequested*>(message.param)->lobby);
            Log("[STEAM] Invite accepted from the Steam friends list\n");
        } else if(message.callback == CallPresenceJoinRequested && message.param) {
            const auto* request = reinterpret_cast<const PresenceJoinRequested*>(message.param);
            const uint64_t id = LobbyFromConnect(std::string(request->connect, strnlen(request->connect, sizeof(request->connect))));
            if(id) joinRequests.push_back(id);
            Log("[STEAM] Join requested from the Steam friends list\n");
        } else if(message.callback == CallSessionRequest && message.param) {
            // Solo chi è nella nostra lobby o un amico può aprire una sessione di rete.
            const auto* identity = reinterpret_cast<const Identity*>(message.param);
            if(identity->type == IdentitySteamID && (LobbyMember(identity->id) || api.hasFriend(api.friends, identity->id, 4))) api.accept(api.messages, identity);
            else Log("[STEAM] Network session from a user outside the lobby ignored\n");
        }
        api.freeLast(pipe);
    }
}
}
bool Initialize(std::string& error) {
    if(const char* test = std::getenv("AMNESIA_NET_TEST_STEAM_FAKE")) {
        // Prove: "utenti" = porte UDP locali, nessun client Steam.
        const unsigned long port = std::strtoul(test, nullptr, 10);
        fakeTransport.reset(online::CreateLoopbackTransport(uint16_t(port)));
        if(!port || port > 65535 || !fakeTransport) { error = "AMNESIA_NET_TEST_STEAM_FAKE: porta non valida"; return false; }
        fake = true; localUser = port;
        if(const char* join = std::getenv("AMNESIA_NET_TEST_STEAM_JOIN")) joinRequests.push_back(std::strtoull(join, nullptr, 10));
        Log("[STEAM] QA loopback Steam on port %lu\n", port);
        return true;
    }
    const char* requested = std::getenv("AMNESIA_STEAM_PRESENCE");
    if(!requested || std::string(requested) != "1") return true;
    if(initialized) return true;
    // Read only the original installation's marker; never write an App ID file.
    std::ifstream marker("steam_appid.txt");
    unsigned int appId = 0; std::string extra;
    if(!(marker >> appId) || appId != AppId || (marker >> extra))
        return Fail(error,"Seleziona l'installazione Steam originale di Amnesia (App ID 57300), oppure disattiva Mostra Amnesia su Steam nel launcher.");
    wchar_t directory[32768];
    const DWORD length = GetCurrentDirectoryW(32768, directory);
    if(!length || length >= 32768) return Fail(error,"Impossibile leggere il percorso dell'installazione Steam.");
    const std::wstring path = std::wstring(directory) + L"\\steam_api64.dll";
    runtime = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!runtime) return Fail(error,"steam_api64.dll originale x64 mancante o non caricabile. Verifica i file del gioco su Steam, oppure disattiva Mostra Amnesia su Steam.");
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(runtime,"SteamAPI_Init"));
    shutdown = reinterpret_cast<VoidFn>(GetProcAddress(runtime,"SteamAPI_Shutdown"));
    runCallbacks = reinterpret_cast<VoidFn>(GetProcAddress(runtime,"SteamAPI_RunCallbacks"));
    const auto apps = reinterpret_cast<InterfaceFn>(GetProcAddress(runtime,"SteamAPI_SteamApps_v008"));
    const auto utils = reinterpret_cast<InterfaceFn>(GetProcAddress(runtime,"SteamAPI_SteamUtils_v010"));
    const auto subscribed = reinterpret_cast<SubscribedFn>(GetProcAddress(runtime,"SteamAPI_ISteamApps_BIsSubscribed"));
    const auto getAppId = reinterpret_cast<AppIdFn>(GetProcAddress(runtime,"SteamAPI_ISteamUtils_GetAppID"));
    if(!init || !shutdown || !apps || !utils || !subscribed || !getAppId)
        return Fail(error,"Runtime Steam originale non supportato. Aggiorna/verifica il gioco su Steam, oppure disattiva Mostra Amnesia su Steam.");
    if(!init()) return Fail(error,"Steam non disponibile: aprilo con l'account che possiede Amnesia. Steam e la mod devono avere gli stessi privilegi Windows. Puoi anche disattivare Mostra Amnesia su Steam.");
    initialized = true;
    void* appInterface = apps(); void* utilsInterface = utils();
    if(!appInterface || !utilsInterface || getAppId(utilsInterface) != AppId || !subscribed(appInterface))
        return Fail(error,"Steam non conferma una licenza attiva di Amnesia: The Dark Descent (57300) per questo account.");
    Log("[STEAM] Registered Amnesia: The Dark Descent (App ID 57300); active license verified\n");
    // Inviti e rete di Steam: se mancano resta solo la presenza su Steam.
    if(LoadApi()) {
        char line[1024] = {};
        if(api.launchLine(api.apps, line, sizeof(line)) > 0) { const uint64_t id = LobbyFromConnect(line); if(id) joinRequests.push_back(id); }
        const uint64_t argument = LobbyFromConnect(GetCommandLineA());
        if(argument) joinRequests.push_back(argument);
        Log("[STEAM] Lobby, invites and relay networking available\n");
    } else Log("[STEAM] Lobby and relay networking not available in this steam_api64.dll\n");
    return true;
}
void Update() {
    if(initialized && api.ready) Dispatch();
    else if(initialized && runCallbacks) runCallbacks(); // runtime senza lobby: solo presenza
}
void Shutdown() {
    if(initialized && api.ready && lobby) { api.leaveLobby(api.matchmaking, lobby); lobby = 0; }
    if(initialized && shutdown) { shutdown(); Log("[STEAM] Shutdown\n"); }
    initialized = false; api = Api(); pipe = 0;
    if(runtime) FreeLibrary(runtime);
    runtime = nullptr; shutdown = runCallbacks = nullptr;
    fakeTransport.reset(); fake = false;
}
bool Available() { return fake || (initialized && api.ready); }
uint64_t LocalUser() { return Available() ? localUser : 0; }
bool OverlayEnabled() { return !fake && Available() && api.overlayEnabled(api.utils); }
bool CreateLobby() {
    if(!Available()) return false;
    if(fake) { lobby = localUser; entered.push_back({lobby, localUser, true}); Log("[STEAM] QA lobby created\n"); return true; }
    if(lobby) { api.leaveLobby(api.matchmaking, lobby); lobby = 0; }
    // Solo amici, fino a 4 giocatori (spazio per le partite a più giocatori).
    return api.createLobby(api.matchmaking, 1, 4) != 0;
}
uint64_t Lobby() { return Available() ? lobby : 0; }
void LeaveLobby() {
    if(!Available() || !lobby) return;
    if(!fake) api.leaveLobby(api.matchmaking, lobby);
    lobby = 0;
    if(!fake) SetPresence();
}
bool JoinLobby(uint64_t id) {
    if(!Available() || !id) return false;
    if(fake) { lobby = id; entered.push_back({id, id, true}); return true; }
    if(lobby && lobby != id) { api.leaveLobby(api.matchmaking, lobby); lobby = 0; }
    return api.joinLobby(api.matchmaking, id) != 0;
}
bool OpenInviteOverlay() {
    if(!OverlayEnabled() || !lobby) return false;
    api.overlayInvite(api.friends, lobby); return true;
}
std::vector<Friend> OnlineFriends() {
    std::vector<Friend> result;
    if(fake) {
        if(const char* other = std::getenv("AMNESIA_NET_TEST_STEAM_FRIEND")) result.push_back({std::strtoull(other, nullptr, 10), "QA friend", true});
        return result;
    }
    if(!Available()) return result;
    const int immediate = 4;
    const int count = api.friendCount(api.friends, immediate);
    for(int i = 0; i < count && i < 500; ++i) {
        const uint64_t id = api.friendByIndex(api.friends, i, immediate);
        if(!id || api.friendState(api.friends, id) == 0) continue; // offline
        FriendGameInfo game = {};
        const bool playing = api.gamePlayed(api.friends, id, &game) && uint32_t(game.game & 0xffffff) == AppId;
        const char* name = api.friendName(api.friends, id);
        result.push_back({id, name ? name : "", playing});
    }
    // Prima chi sta già giocando ad Amnesia, poi in ordine di nome.
    std::sort(result.begin(), result.end(), [](const Friend& a, const Friend& b) {
        if(a.playing != b.playing) return a.playing;
        return a.name < b.name;
    });
    return result;
}
bool Invite(uint64_t user) {
    if(!Available() || !lobby || !user) return false;
    if(fake) { Log("[STEAM] QA invite sent to %llu\n", (unsigned long long)user); return true; }
    return api.invite(api.matchmaking, lobby, user);
}
std::string UserName(uint64_t user) {
    if(fake) return "QA " + std::to_string(user);
    if(!Available() || !user) return "";
    const char* name = api.friendName(api.friends, user);
    return name ? name : "";
}
bool TakeJoinRequest(uint64_t& id) {
    if(joinRequests.empty()) return false;
    id = joinRequests.front(); joinRequests.pop_front(); return id != 0;
}
bool TakeLobbyEntered(uint64_t& id, uint64_t& owner, bool& ok) {
    if(entered.empty()) return false;
    id = entered.front().lobby; owner = entered.front().owner; ok = entered.front().ok; entered.pop_front(); return true;
}
online::MessageTransport* Transport() {
    if(fake) return fakeTransport.get();
    return Available() ? static_cast<online::MessageTransport*>(&steamTransport) : nullptr;
}
}
#else
namespace luxsteam {
bool Initialize(std::string&) { return true; }
void Update() {}
void Shutdown() {}
bool Available() { return false; }
uint64_t LocalUser() { return 0; }
bool OverlayEnabled() { return false; }
bool CreateLobby() { return false; }
uint64_t Lobby() { return 0; }
void LeaveLobby() {}
bool JoinLobby(uint64_t) { return false; }
bool OpenInviteOverlay() { return false; }
std::vector<Friend> OnlineFriends() { return {}; }
bool Invite(uint64_t) { return false; }
std::string UserName(uint64_t) { return ""; }
bool TakeJoinRequest(uint64_t&) { return false; }
bool TakeLobbyEntered(uint64_t&, uint64_t&, bool&) { return false; }
online::MessageTransport* Transport() { return nullptr; }
}
#endif
