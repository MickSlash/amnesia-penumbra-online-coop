// SPDX-License-Identifier: GPL-3.0-or-later
#include "CoopSteam.h"
#include "StdAfx.h"
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <cstdlib>
void CoopSteam::Start() {
    const char* enabled=getenv("PENUMBRA_COOP_STEAM");
    if(!enabled || std::string(enabled)!="1") return;
    const wchar_t* path=_wgetenv(L"PENUMBRA_COOP_STEAM_API");
    if(!path || !*path) hpl::FatalError("Runtime Steam mancante. Disattiva Usa Steam nel launcher.\n");
    WINTRUST_FILE_INFO file={}; file.cbStruct=sizeof(file); file.pcwszFilePath=path;
    WINTRUST_DATA trust={}; trust.cbStruct=sizeof(trust); trust.dwUIChoice=WTD_UI_NONE;
    trust.dwUnionChoice=WTD_CHOICE_FILE; trust.pFile=&file; trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    GUID action=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    if(WinVerifyTrust(nullptr,&action,&trust)!=ERROR_SUCCESS) hpl::FatalError("La firma del runtime Steam non e' valida. Disattiva Usa Steam nel launcher.\n");
    auto module=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module) hpl::FatalError("Impossibile caricare Steam a 32 bit. Disattiva Usa Steam nel launcher.\n");
    auto initialize=reinterpret_cast<bool(*)()>(GetProcAddress(module,"SteamAPI_Init"));
    auto pump=reinterpret_cast<void(*)()>(GetProcAddress(module,"SteamAPI_RunCallbacks"));
    auto stop=reinterpret_cast<void(*)()>(GetProcAddress(module,"SteamAPI_Shutdown"));
    if(!initialize || !pump || !stop || !initialize()) {
        FreeLibrary(module);
        hpl::FatalError("Steam non ha accettato Penumbra: Overture (22180). Apri Steam con l'account che possiede il gioco, oppure disattiva Usa Steam.\n");
    }
    callbacks=pump; shutdown=stop;
    hpl::Log("[COOP/STEAM] SteamAPI_Init accepted App ID 22180; presence enabled\n");
    // Steam's overlay can retain rendering hooks until process exit; keep the module loaded.
}
void CoopSteam::Update() { if(callbacks) callbacks(); }
CoopSteam::~CoopSteam() { if(shutdown) { shutdown(); hpl::Log("[COOP/STEAM] SteamAPI_Shutdown\n"); } }
