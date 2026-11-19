// SPDX-License-Identifier: GPL-3.0-or-later
// Included by LuxMainMenu.cpp, using the existing native HPL GUI.
// Testi in inglese; in italiano se il gioco è in italiano.
inline const wchar_t* OnlineText(const wchar_t* english, const wchar_t* italian) { return cLuxNetwork::ItalianText() ? italian : english; }
inline const char* OnlineText(const char* english, const char* italian) { return cLuxNetwork::ItalianText() ? italian : english; }
class cLuxMainMenu_Online : public iLuxMainMenuWindow {
public:
    cLuxMainMenu_Online(cGuiSet* set, cGuiSkin* skin) : iLuxMainMenuWindow(set, skin) {
        savedAddress = gpBase->mpMainConfig->GetString("Online","LastAddress","127.0.0.1");
        savedPort = gpBase->mpMainConfig->GetString("Online","Port","27020");
    }
    void ExitPressed() { gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_LastEnum); }
    void CreateGui() {
        mpWindow = mpGuiSet->CreateWidgetWindow(eWidgetWindowButtonFlag_None, cVector3f(0,0,5), cVector2f(620,410), OnlineText(L"Online co-op campaign", L"Campagna cooperativa online"));
        mpWindow->AddCallback(eGuiMessage_OnUpdate, this, kGuiCallback(UpdateWindow));
        mpGuiSet->CreateWidgetLabel(cVector3f(20,40,0.1f), cVector2f(570,24), OnlineText(L"LAN / Hamachi / Radmin: use the IP of the PC that hosts.", L"LAN / Hamachi / Radmin: usa l'IP del PC che ospita."), mpWindow);
        mpGuiSet->CreateWidgetLabel(cVector3f(20,76,0.1f), cVector2f(180,24), OnlineText(L"Host IP", L"IP dell'host"), mpWindow);
        address = mpGuiSet->CreateWidgetTextBox(cVector3f(200,72,0.1f), cVector2f(240,30), cString::To16Char(savedAddress), mpWindow);
        mpGuiSet->CreateWidgetLabel(cVector3f(450,76,0.1f), cVector2f(60,24), OnlineText(L"Port", L"Porta"), mpWindow);
        port = mpGuiSet->CreateWidgetTextBox(cVector3f(510,72,0.1f), cVector2f(90,30), cString::To16Char(savedPort), mpWindow);
        const wchar_t* labels[] = {OnlineText(L"Host", L"Ospita"), OnlineText(L"Connect", L"Connetti"), OnlineText(L"Reconnect", L"Riconnettiti"),
            OnlineText(L"Ready", L"Pronto"), OnlineText(L"New campaign", L"Nuova campagna"), OnlineText(L"Continue saved game", L"Continua salvataggio"),
            OnlineText(L"Close session", L"Chiudi sessione"), OnlineText(L"Back", L"Indietro")};
        for(int i = 0; i < 8; ++i) {
            float x = i < 4 ? 20.0f + i*145 : i < 6 ? 20.0f + (i-4)*290 : 20.0f + (i-6)*290;
            float y = i < 4 ? 115.0f : i < 6 ? 157.0f : 360.0f;
            buttons[i] = mpGuiSet->CreateWidgetButton(cVector3f(x,y,0.1f), cVector2f(i < 4 ? 140.0f : 275.0f,32), labels[i], mpWindow);
            buttons[i]->SetUserValue(i); buttons[i]->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(Press));
            if(i == 2) buttons[i]->SetName("OnlineReconnect");
            if(i == 4) buttons[i]->SetName("OnlineNewCampaign");
        }
        clearImage = mpGuiSet->CreateWidgetCheckBox(cVector3f(20,204,0.1f), cVector2f(570,24), OnlineText(L"Clear image (less motion trails and blur)", L"Immagine nitida (riduci scie e blur)"), mpWindow);
        clearImage->SetName("OnlineClearImage");
        clearImage->SetChecked(gpBase->mpNetwork->ClearImagePreference(),false);
        clearImage->AddCallback(eGuiMessage_CheckChange,this,kGuiCallback(ChangeImage));
        status = mpGuiSet->CreateWidgetLabel(cVector3f(20,239,0.1f), cVector2f(580,58), L"", mpWindow);
        status->SetWordWrap(true);
        // Inviti Steam: amici online (prima chi gioca ad Amnesia) e, se c'è,
        // la finestra di invito dell'overlay di Steam.
        friends = mpGuiSet->CreateWidgetComboBox(cVector3f(20,312,0.1f), cVector2f(400,28), L"", mpWindow);
        friends->SetName("OnlineSteamFriends");
        invite = mpGuiSet->CreateWidgetButton(cVector3f(430,310,0.1f), cVector2f(170,32), OnlineText(L"Invite", L"Invita"), mpWindow);
        invite->SetName("OnlineSteamInvite");
        invite->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(PressInvite));
        RefreshFriends();
    }
private:
    cWidgetTextBox *address = NULL, *port = NULL;
    cWidgetLabel* status = NULL;
    cWidgetButton* buttons[8] = {};
    cWidgetCheckBox* clearImage = NULL;
    cWidgetComboBox* friends = NULL;
    cWidgetButton* invite = NULL;
    std::vector<uint64_t> friendIds; // 0 = overlay di Steam
    bool lobbyWasOpen = false;
    void RefreshFriends() {
        friends->ClearItems(); friendIds.clear();
        if(!gpBase->mpNetwork->SteamAvailable()) {
            friends->AddItem(OnlineText(L"Steam invites: start with \"Show Amnesia on Steam\"", L"Inviti Steam: avvia con \"Mostra Amnesia su Steam\""));
            friendIds.push_back(0); friends->SetSelectedItem(0, false, false); return;
        }
        if(luxsteam::OverlayEnabled()) { friends->AddItem(OnlineText(L"Steam overlay: choose friends...", L"Overlay di Steam: scegli gli amici...")); friendIds.push_back(0); }
        for(const auto& person : luxsteam::OnlineFriends()) {
            tWString name = cString::UTF8ToWChar(person.name);
            if(person.playing) name += OnlineText(L"  (in Amnesia)", L"  (in Amnesia)");
            friends->AddItem(name); friendIds.push_back(person.id);
        }
        if(friendIds.empty()) { friends->AddItem(OnlineText(L"No Steam friends online", L"Nessun amico Steam online")); friendIds.push_back(0); }
        friends->SetSelectedItem(0, false, false);
    }
    bool PressInvite(iWidget*, const cGuiMessageData&) {
        error.clear();
        if(!gpBase->mpNetwork->SteamLobbyOpen()) { error = OnlineText("Press Host first: the Steam lobby opens with the session.", "Premi prima Ospita: la lobby Steam si apre con la sessione."); return true; }
        const int selected = friends->GetSelectedItem();
        if(selected < 0 || selected >= int(friendIds.size())) return true;
        const uint64_t id = friendIds[selected];
        if(!id) {
            if(!luxsteam::OpenInviteOverlay()) { RefreshFriends(); error = OnlineText("Steam overlay unavailable: choose a friend from the list.", "Overlay di Steam non disponibile: scegli un amico dalla lista."); }
            return true;
        }
        const std::string name = luxsteam::UserName(id);
        gpBase->mpNetwork->SetSteamNotice(luxsteam::Invite(id) ?
            std::string(OnlineText("Invite sent to ", "Invito inviato a ")) + name + OnlineText(". They accept it from Steam with the game open.", ". Lo accetta da Steam con il gioco aperto.") :
            std::string(OnlineText("Steam did not send the invite.", "Steam non ha inviato l'invito.")));
        return true;
    }
    kGuiCallbackDeclarationEnd(PressInvite);
    tString savedAddress = "127.0.0.1", savedPort = "27020", error;
    bool UpdateWindow(iWidget*, const cGuiMessageData&) {
        auto& session = gpBase->mpNetwork->GetSession();
        const bool offline = session.GetRole() == online::Role::Offline;
        buttons[0]->SetEnabled(offline); buttons[1]->SetEnabled(offline);
        address->SetEnabled(offline); port->SetEnabled(offline);
        buttons[2]->SetEnabled(gpBase->mpNetwork->CanReconnect());
        buttons[3]->SetEnabled(session.Connected());
        buttons[3]->SetText(session.Connected() && session.LocalReady() ? OnlineText(L"Not ready", L"Non pronto") : OnlineText(L"Ready", L"Pronto"));
        const bool canStart = session.GetRole() == online::Role::Host && session.LobbyReady() && !gpBase->mpMapHandler->MapIsLoaded();
        buttons[4]->SetEnabled(canStart);
        buttons[5]->SetEnabled(canStart && gpBase->mpSaveHandler->SaveFileExists());
        buttons[6]->SetEnabled(!offline);
        const bool lobbyOpen = gpBase->mpNetwork->SteamLobbyOpen();
        if(lobbyOpen && !lobbyWasOpen) RefreshFriends();
        lobbyWasOpen = lobbyOpen;
        friends->SetEnabled(gpBase->mpNetwork->SteamAvailable());
        invite->SetEnabled(lobbyOpen);
        std::string text = error.empty() ? gpBase->mpNetwork->ConnectionStatus() : error;
        if(error.empty() && lobbyOpen && !gpBase->mpNetwork->SteamNotice().empty() && session.GetRole() == online::Role::Host && !session.Connected())
            text = gpBase->mpNetwork->SteamNotice();
        status->SetText(cString::UTF8ToWChar(text));
        return true;
    }
    kGuiCallbackDeclarationEnd(UpdateWindow);
    bool ChangeImage(iWidget*,const cGuiMessageData&) {
        gpBase->mpNetwork->SetClearImage(clearImage->IsChecked()); return true;
    }
    kGuiCallbackDeclarationEnd(ChangeImage);
    bool Press(iWidget* widget, const cGuiMessageData&) {
        const int action = widget->GetUserValue(); error.clear();
        savedAddress = cString::To8Char(address->GetText()); savedPort = cString::To8Char(port->GetText());
        char* end = NULL; const unsigned long value = std::strtoul(savedPort.c_str(), &end, 10);
        if(action < 2 && (savedPort.empty() || !end || *end || value < 1 || value > 65535 || savedAddress.empty())) { error = OnlineText("Enter a valid IP and port (1-65535).", "Inserisci un IP e una porta valida (1-65535)."); return true; }
        auto& session = gpBase->mpNetwork->GetSession();
        if(action == 0 || action == 1) {
            session.SetLobbyReady(false);
            bool ok = action == 0 ? gpBase->mpNetwork->Host(uint16_t(value)) : gpBase->mpNetwork->Join(savedAddress, uint16_t(value));
            if(!ok) error = OnlineText("Could not start the session. Check the IP and that the port is free.", "Impossibile avviare la sessione. Controlla IP e porta libera.");
        } else if(action == 2) {
            if(!gpBase->mpNetwork->Reconnect()) error = OnlineText("Reconnect failed. Check the last session and the port.", "Riconnessione non riuscita. Controlla l'ultima sessione e la porta.");
        } else if(action == 3) session.SetLobbyReady(!session.LocalReady());
        else if(action == 4 || action == 5) {
            if(session.GetRole() != online::Role::Host || !session.LobbyReady()) return true;
            gpBase->mbCoopWanted = false; gpBase->mbHardMode = false; gpBase->SetCustomStory(NULL);
            gpBase->mpMainMenu->ExitMenu(action == 4 ? eLuxMainMenuExit_StartGame : eLuxMainMenuExit_ContinueGame);
        } else if(action == 6) gpBase->mpNetwork->Disconnect();
        else ExitPressed();
        return true;
    }
    kGuiCallbackDeclarationEnd(Press);
};
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Online, UpdateWindow);
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Online, ChangeImage);
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Online, Press);
kGuiCallbackDeclaredFuncEnd(cLuxMainMenu_Online, PressInvite);
