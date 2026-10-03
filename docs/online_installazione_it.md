# Installare e giocare ad Amnesia Online Co-op

Build sperimentale Windows x64, protocollo 5, due giocatori. Entrambi devono possedere Amnesia: The Dark Descent, usare gli stessi dati e la stessa build. La campagna intera non è ancora certificata; le prove sono in `docs/qa_recovery_2026-10-03.md` dentro `source.zip`.

## Avvio senza PowerShell

1. Estrarre **tutto lo ZIP** in una cartella propria. Conservare `AmnesiaCoop.exe`, `bin` e gli altri file insieme.
2. Fare doppio clic su **AmnesiaCoop.exe**. Scegliere, se necessario, la cartella dell'installazione originale contenente `config`, `maps` ed `entities`. Con la copia Steam, aprire Steam con l'account che possiede il gioco e lasciare selezionato **Mostra Amnesia su Steam**. Premere **Avvia Amnesia co-op**. Il launcher ricorda il percorso.
3. Nel gioco scegliere **Co-op online**. L'host preme **Ospita**; l'altro inserisce l'IPv4 dell'host e preme **Connetti**. Porta predefinita: **27020 UDP**.
4. Entrambi premono **Pronto**. L'host sceglie **Nuova campagna** oppure **Continua salvataggio**. Il client carica automaticamente il livello.

Tra PC diversi usare l'IPv4 LAN o del tunnel VPN; `127.0.0.1` serve solo per due istanze sullo stesso PC. Non selezionare la modalità co-op locale/split-screen. Si gioca con tastiera e mouse, con i controlli configurati nel gioco, nella propria visuale. Il menu online usa il mouse; la navigazione completa con gamepad non è stata verificata. **Chiudi sessione** termina volontariamente la rete e disattiva il recupero automatico.

Non occorrono PowerShell né Visual Studio per giocare. Lo script in `tools` rimane un'alternativa per sviluppo. Il launcher non sostituisce l'eseguibile Steam e non modifica i dati del gioco: scrive configurazioni separate in AppData. `manifest.json` contiene gli hash; `source.zip` i sorgenti e le licenze. Il pacchetto contiene OpenAL x64 e il relativo backend quando presente.

Se manca `MSVCP140.dll`, `VCRUNTIME140.dll` o `VCRUNTIME140_1.dll`, installare il [Microsoft Visual C++ Redistributable x64](https://aka.ms/vc14/vc_redist.x64.exe). Serve un driver grafico compatibile con OpenGL.

## Riconoscimento su Steam

**Mostra Amnesia su Steam** viene selezionato automaticamente quando il launcher trova `steam_api64.dll` e il marcatore App ID 57300 nella cartella originale. La mod carica quella DLL a runtime, verifica l'App ID e la licenza attiva dell'account, ed esegue i callback Steam durante il gioco. Steam mostra **Amnesia: The Dark Descent — In esecuzione**. Avviare sempre dal nuovo `AmnesiaCoop.exe`: il pulsante Gioca della libreria Steam continua ad avviare il gioco originale.

Steam deve essere aperto e la mod deve avere gli stessi privilegi Windows del client Steam: normalmente avviare entrambi senza **Esegui come amministratore**. Se il runtime manca, verificare i file di Amnesia dalla libreria Steam. L'avvio segnala un errore se l'integrazione richiesta non può essere inizializzata; non passa silenziosamente a una presenza diversa.

Per giocare senza Steam, con un'altra distribuzione legittima o offline senza il client, deselezionare la casella. La rete co-op resta su IP/UDP. Questa integrazione registra l'attività di gioco: non aggiunge achievement, sincronizzazione dei salvataggi Steam Cloud, inviti Steam o matchmaking. È comparsa la notifica dell'overlay nel test locale; l'apertura con Shift+Tab non è stata confermata dall'automazione. I salvataggi co-op restano nel profilo separato descritto sotto. Nessuna DLL Steam viene inclusa nello ZIP. Dettagli delle prove in `docs/qa_steam_2026-10-03.md` dentro `source.zip`.

## LAN, Hamachi e Radmin VPN

Entrare nella stessa LAN o rete virtuale. Nel campo IP usare l'IPv4 LAN, Hamachi o Radmin dell'host. La mod ascolta su tutte le interfacce IPv4 e usa UDP: queste VPN sono compatibili con il trasporto se il tunnel permette la comunicazione tra i PC. **Una partita su due PC/VPN non è stata verificata.**

Consentire `bin\Lux.exe` nel firewall e la porta UDP scelta **su entrambi i PC**, perché il client può diventare host durante il recupero. Lasciare il firewall attivo. Usare la stessa porta e dati identici. Non sono inclusi ricerca automatica dei server, matchmaking, relay o NAT traversal. Per Internet diretto serve una porta raggiungibile; per la migrazione anche il precedente client deve essere raggiungibile dall'altro PC.

Questa build limita i datagrammi ENet a **1200 byte** per lasciare spazio all'incapsulamento delle VPN. Il limite è negoziato durante la connessione; i messaggi più grandi vengono frammentati da ENet. Nei test locali la correzione permette di trasferire lo stato iniziale e i checkpoint anche quando la rete scarta tutti i datagrammi sopra 1200 byte. Non è una misura della MTU effettiva di Radmin o Hamachi, né una garanzia su ogni tunnel. Per aggiornare, estrarre il nuovo ZIP in una cartella nuova e avviare il launcher da lì **su entrambi i PC**; non eliminare i profili o i salvataggi. Diagnosi e confronto prima/dopo in `docs/qa_radmin_diagnosi_2026-10-03.md` dentro `source.zip`.

## Salvataggi, journal e recupero

L'host salva campagna e stato di entrambi sotto `Documenti\Amnesia\AmnesiaOnlineCoop_Host\online`. Il launcher grafico usa questa cartella su ciascun PC, anche come client, per permettere il cambio di autorità. I vecchi profili dello script restano nella propria sottocartella: selezionarli con **Change Profile** per i rispettivi salvataggi.

Per una partita chiusa normalmente, ricollegarsi dalla lobby, premere Pronto su entrambi e far scegliere Continua all'host. Il journal trasferisce note, ordine dei diari e obiettivi attivi/completati anche al join e al caricamento, senza riaprire automaticamente documenti già raccolti.

La perdita di connessione mette la campagna in pausa e avvia tentativi automatici di riconnessione. Se l'host resta assente e il client ha un checkpoint completo e validato, il client può diventare host dopo circa 12 secondi. Ripristina il salvataggio nativo, inverte correttamente i giocatori, esegue un autosalvataggio e **aspetta il partner**. Non prosegue da solo.

I checkpoint sono catturati ogni 20 secondi, oltre al primo stato completo e ai cambi mappa. Si possono perdere i progressi successivi all'ultimo checkpoint ricevuto, con ritardo aggiuntivo dovuto al trasferimento. Il vecchio host ancora aperto prova a raggiungere il nuovo automaticamente. Dopo un crash e riavvio, collegarsi all'IP del nuovo host con lo stesso profilo per conservare l'identità. Non eliminare configurazioni/profili durante la sessione. La migrazione richiede checkpoint valido e raggiungibilità in entrambe le direzioni; non è senza pausa.

Inventario e progressione sono condivisi. Olio e attivazione della lanterna sono individuali nella campagna originale. Il partner vede luce e posa dell'avatar; non è incluso un modello dettagliato della lanterna in terza persona. I nemici possono scegliere entrambi, inseguono un bersaglio per volta e verificano separatamente la visibilità. Nascondersi rompe la visuale, ma possono cercare nell'ultima posizione conosciuta.

## Limiti ancora aperti

- Campagna completa, puzzle, inseguimenti, finali e Hard da verificare su due PC. Le prove coprono integrazioni e alcuni livelli, non un'intera partita.
- Prediction locale limitata al movimento orizzontale, con collisioni e correzione dell'host. Salti, crouch e oggetti fisici restano autorevoli; reti lente possono ancora causare correzioni visibili.
- Migrazione provata tra processi locali; crash del PC, riapertura del gioco, partizioni prolungate e WAN richiedono ulteriori prove.
- Con 100 ms simulati e perdita periodica del 5%, una regressione ha rilevato una caduta durante il cambio livello: il recupero ricostruisce il mondo, ma il client non osserva l'intera sequenza di morte/ripresa. Il limite dei datagrammi risolve il caso di perdita per dimensione verificato, ma non certifica la stabilità con forte perdita di pacchetti.
- Alcuni effetti script, creazione/trasformazione delle luci e tipi di entità fuori dal roster richiedono copertura. Audio e particelle replicano stato e ciclo di vita, non ogni campione sonoro o il seme casuale degli emettitori.

Gli errori sono nel `hpl.log` della cartella del profilo. Se non si collega, controllare IP, porta, firewall, build e dati. La console sviluppatore conserva `net_status` e `net_ping`, ma non serve per iniziare una partita.

I nuovi messaggi di connessione riportano `[t=...ms]` dal contatore monotono del singolo PC, la dimensione negoziata dei datagrammi, il codice ricevuto alla disconnessione e il tempo dall'ultimo pacchetto applicativo. I contatori dei due PC non hanno la stessa origine. `[NET/DIAG] Transport service paused` segnala un intervallo superiore a un secondo senza servizio della rete sul thread del gioco, possibile anche durante un caricamento: da solo non identifica la causa. Se le cadute persistono, conservare i log di entrambi prima di riavviare il gioco.
