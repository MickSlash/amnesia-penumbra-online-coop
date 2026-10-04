// SPDX-License-Identifier: GPL-3.0-or-later
// Included by LuxMainMenu.cpp, using the existing native HPL GUI.
class cLuxMainMenu_Online : public iLuxMainMenuWindow {
public:
    cLuxMainMenu_Online(cGuiSet* set, cGuiSkin* skin) : iLuxMainMenuWindow(set, skin) {
        savedAddress = gpBase->mpMainConfig->GetString("Online","LastAddress","127.0.0.1");
        savedPort = gpBase->mpMainConfig->GetString("Online","Port","27020");
    }
    void ExitPressed() { gpBase->mpMainMenu->SetWindowActive(eLuxMainMenuWindow_LastEnum); }
    void CreateGui() {
        mpWindow = mpGuiSet->CreateWidgetWindow(eWidgetWindowButtonFlag_None, cVector3f(0,0,5), cVector2f(620,360), L"Campagna cooperativa online");
        mpWindow->AddCallback(eGuiMessage_OnUpdate, this, kGuiCallback(UpdateWindow));
        mpGuiSet->CreateWidgetLabel(cVector3f(20,40,0.1f), cVector2f(570,24), L"LAN / Hamachi / Radmin: usa l'IP del PC che ospita.", mpWindow);
        mpGuiSet->CreateWidgetLabel(cVector3f(20,76,0.1f), cVector2f(180,24), L"IP dell'host", mpWindow);
        address = mpGuiSet->CreateWidgetTextBox(cVector3f(200,72,0.1f), cVector2f(240,30), cString::To16Char(savedAddress), mpWindow);
        mpGuiSet->CreateWidgetLabel(cVector3f(450,76,0.1f), cVector2f(60,24), L"Porta", mpWindow);
        port = mpGuiSet->CreateWidgetTextBox(cVector3f(510,72,0.1f), cVector2f(90,30), cString::To16Char(savedPort), mpWindow);
        const wchar_t* labels[] = {L"Ospita", L"Connetti", L"Riconnettiti", L"Pronto", L"Nuova campagna", L"Continua salvataggio", L"Chiudi sessione", L"Indietro"};
        for(int i = 0; i < 8; ++i) {
            float x = i < 4 ? 20.0f + i*145 : i < 6 ? 20.0f + (i-4)*290 : 20.0f + (i-6)*290;
            float y = i < 4 ? 115.0f : i < 6 ? 157.0f : 310.0f;
            buttons[i] = mpGuiSet->CreateWidgetButton(cVector3f(x,y,0.1f), cVector2f(i < 4 ? 140.0f : 275.0f,32), labels[i], mpWindow);
            buttons[i]->SetUserValue(i); buttons[i]->AddCallback(eGuiMessage_ButtonPressed, this, kGuiCallback(Press));
            if(i == 2) buttons[i]->SetName("OnlineReconnect");
        }
        clearImage = mpGuiSet->CreateWidgetCheckBox(cVector3f(20,204,0.1f), cVector2f(570,24), L"Immagine nitida (riduci scie e blur)", mpWindow);
        clearImage->SetName("OnlineClearImage");
        clearImage->SetChecked(gpBase->mpNetwork->ClearImagePreference(),false);
        clearImage->AddCallback(eGuiMessage_CheckChange,this,kGuiCallback(ChangeImage));
        status = mpGuiSet->CreateWidgetLabel(cVector3f(20,239,0.1f), cVector2f(580,58), L"", mpWindow);
        status->SetWordWrap(true);
    }
private:
    cWidgetTextBox *address = NULL, *port = NULL;
    cWidgetLabel* status = NULL;
    cWidgetButton* buttons[8] = {};
    cWidgetCheckBox* clearImage = NULL;
    tString savedAddress = "127.0.0.1", savedPort = "27020", error;
    bool UpdateWindow(iWidget*, const cGuiMessageData&) {
        auto& session = gpBase->mpNetwork->GetSession();
        const bool offline = session.GetRole() == online::Role::Offline;
        buttons[0]->SetEnabled(offline); buttons[1]->SetEnabled(offline);
        address->SetEnabled(offline); port->SetEnabled(offline);
        buttons[2]->SetEnabled(gpBase->mpNetwork->CanReconnect());
        buttons[3]->SetEnabled(session.Connected());
        buttons[3]->SetText(session.Connected() && session.LocalReady() ? L"Non pronto" : L"Pronto");
        const bool canStart = session.GetRole() == online::Role::Host && session.LobbyReady() && !gpBase->mpMapHandler->MapIsLoaded();
        buttons[4]->SetEnabled(canStart);
        buttons[5]->SetEnabled(canStart && gpBase->mpSaveHandler->SaveFileExists());
        buttons[6]->SetEnabled(!offline);
        status->SetText(cString::To16Char(error.empty() ? gpBase->mpNetwork->ConnectionStatus() : error));
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
        if(action < 2 && (savedPort.empty() || !end || *end || value < 1 || value > 65535 || savedAddress.empty())) { error = "Inserisci un IP e una porta valida (1-65535)."; return true; }
        auto& session = gpBase->mpNetwork->GetSession();
        if(action == 0 || action == 1) {
            session.SetLobbyReady(false);
            bool ok = action == 0 ? gpBase->mpNetwork->Host(uint16_t(value)) : gpBase->mpNetwork->Join(savedAddress, uint16_t(value));
            if(!ok) error = "Impossibile avviare la sessione. Controlla IP e porta libera.";
        } else if(action == 2) {
            if(!gpBase->mpNetwork->Reconnect()) error = "Riconnessione non riuscita. Controlla l'ultima sessione e la porta.";
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
