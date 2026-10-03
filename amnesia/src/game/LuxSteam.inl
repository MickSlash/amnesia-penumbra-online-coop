// SPDX-License-Identifier: GPL-3.0-or-later
// Compiled by LuxBase.cpp. No SDK or Valve runtime is redistributed.
#include "LuxSteam.h"
#include <cstdlib>
#include <fstream>
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
VoidFn shutdown = nullptr, callbacks = nullptr;
bool Fail(std::string& error, const char* message) {
    error = message;
    Log("[STEAM] Initialization failed: %s\n", message);
    Shutdown();
    return false;
}
}
bool Initialize(std::string& error) {
    const char* requested = std::getenv("AMNESIA_STEAM_PRESENCE");
    if(!requested || std::string(requested) != "1") return true;
    if(initialized) return true;
    // Read only the original installation's marker; never write an App ID file.
    std::ifstream marker("steam_appid.txt");
    unsigned int appId = 0; std::string extra;
    if(!(marker >> appId) || appId != 57300 || (marker >> extra))
        return Fail(error,"Seleziona l'installazione Steam originale di Amnesia (App ID 57300), oppure disattiva Mostra Amnesia su Steam nel launcher.");
    wchar_t directory[32768];
    const DWORD length = GetCurrentDirectoryW(32768, directory);
    if(!length || length >= 32768) return Fail(error,"Impossibile leggere il percorso dell'installazione Steam.");
    const std::wstring path = std::wstring(directory) + L"\\steam_api64.dll";
    runtime = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!runtime) return Fail(error,"steam_api64.dll originale x64 mancante o non caricabile. Verifica i file del gioco su Steam, oppure disattiva Mostra Amnesia su Steam.");
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(runtime,"SteamAPI_Init"));
    shutdown = reinterpret_cast<VoidFn>(GetProcAddress(runtime,"SteamAPI_Shutdown"));
    callbacks = reinterpret_cast<VoidFn>(GetProcAddress(runtime,"SteamAPI_RunCallbacks"));
    const auto apps = reinterpret_cast<InterfaceFn>(GetProcAddress(runtime,"SteamAPI_SteamApps_v008"));
    const auto utils = reinterpret_cast<InterfaceFn>(GetProcAddress(runtime,"SteamAPI_SteamUtils_v010"));
    const auto subscribed = reinterpret_cast<SubscribedFn>(GetProcAddress(runtime,"SteamAPI_ISteamApps_BIsSubscribed"));
    const auto getAppId = reinterpret_cast<AppIdFn>(GetProcAddress(runtime,"SteamAPI_ISteamUtils_GetAppID"));
    if(!init || !shutdown || !callbacks || !apps || !utils || !subscribed || !getAppId)
        return Fail(error,"Runtime Steam originale non supportato. Aggiorna/verifica il gioco su Steam, oppure disattiva Mostra Amnesia su Steam.");
    if(!init()) return Fail(error,"Steam non disponibile: aprilo con l'account che possiede Amnesia. Steam e la mod devono avere gli stessi privilegi Windows. Puoi anche disattivare Mostra Amnesia su Steam.");
    initialized = true;
    void* appInterface = apps(); void* utilsInterface = utils();
    if(!appInterface || !utilsInterface || getAppId(utilsInterface) != 57300 || !subscribed(appInterface))
        return Fail(error,"Steam non conferma una licenza attiva di Amnesia: The Dark Descent (57300) per questo account.");
    Log("[STEAM] Registered Amnesia: The Dark Descent (App ID 57300); active license verified\n");
    return true;
}
void Update() { if(initialized && callbacks) callbacks(); }
void Shutdown() {
    if(initialized && shutdown) { shutdown(); Log("[STEAM] Shutdown\n"); }
    initialized = false;
    if(runtime) FreeLibrary(runtime);
    runtime = nullptr; shutdown = callbacks = nullptr;
}
}
#else
namespace luxsteam {
bool Initialize(std::string&) { return true; }
void Update() {}
void Shutdown() {}
}
#endif
