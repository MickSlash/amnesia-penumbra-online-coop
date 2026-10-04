# Installare e giocare ad Amnesia Online Co-op

Build sperimentale Windows x64, protocollo 10, due giocatori. Entrambi devono possedere Amnesia: The Dark Descent, usare gli stessi dati e la stessa build. La campagna intera non è ancora certificata; le prove sono nei documenti QA dentro `source.zip`.

La versione 9 corregge il recupero dopo il blocco dell'avvio riscontrato nella
versione 8: l'host mantiene la campagna in pausa e il listener aperto. Se il
trasferimento iniziale resta bloccato, la riconnessione automatica negozia
datagrammi più piccoli. Le copie affidabili del movimento non si accumulano;
i checkpoint viaggiano in porzioni più piccole. Gli avatar vengono nascosti
solo durante l'intro già sincronizzata. Prove e limiti in
`docs/qa_blocco_remoto_2026-10-04.md` dentro `source.zip`.

L'avvio della campagna avvisa il partner prima del
caricamento e concede una finestra di rete dedicata, poi attende che il client
abbia applicato il mondo completo prima di avviare i timer della storia.
Durante l'intro del primo livello il client condivide la telecamera dell'host,
con voce, sottotitoli ed effetti; gli avatar non coprono la scena. Al termine
torna alla propria visuale e ai controlli indipendenti. Le due copie devono
essere aggiornate: le build precedenti vengono rifiutate alla connessione.
Prove e limiti in `docs/qa_avvio_campagna_2026-10-04.md` dentro `source.zip`.

Questa build corregge anche la camminata continua del client dopo il rilascio dei tasti: aggiornare **entrambi**, incluso l'host. La prova dell'arresto avanti/laterale è documentata in `docs/qa_movimento_2026-10-03.md` dentro `source.zip`.

Riduce inoltre le pause periodiche: validazione dei checkpoint client e stampa/compressione host vengono eseguite in background. Sono ottimizzate le scansioni di audio/particelle e corretti i campi non inizializzati delle particelle assenti nei salvataggi. Misure e prove in `docs/qa_lag_cpu_2026-10-04.md` dentro `source.zip`; la scomparsa di ogni rallentamento sui PC remoti richiede ancora una prova con questa build.

La costruzione XML dei checkpoint host viene ora distribuita su più frame con
un budget di 1,5 ms per passaggio. Il client ripristina i precaricamenti letterali
di suoni e particelle previsti dallo script durante il caricamento e distribuisce
la replica del mondo in piccoli gruppi, conservando gli eventi in coda.
Corregge un conflitto nella rimozione delle particelle sul client e aggiunge una
copia dello stato di riserva che supera un pacchetto affidabile mancante,
continuando a respingere stati vecchi e duplicati.
Prove e limiti in `docs/qa_micro_scatti_2026-10-04.md` dentro `source.zip`.

La versione 6 corregge la posizione della lanterna in prima persona sul client e l'applicazione delle lanterne autorevoli anche con permessi locali obsoleti. Invia snapshot a 60 Hz, aggiunge stati affidabili di riserva ogni 100 ms e quando una lanterna cambia stato, senza riavviare prediction o interpolazione. Audio ed entità restano a 20 Hz. Riassorbe gradualmente le piccole correzioni del movimento e rinvia i dati del mondo quando ci sono troppe informazioni in attesa o ritrasmissioni. Corregge inoltre il limite di ricezione di 256 KiB che poteva impedire il recupero dei pacchetti mancanti durante il cambio livello. **Aggiornare entrambi i PC**: le build del protocollo 5 vengono rifiutate alla connessione. Prove e limiti in `docs/qa_lanterne_snapshot_2026-10-04.md` dentro `source.zip`.

Corregge anche il throttle ENet che, dopo picchi degli ACK, poteva scartare tutti gli snapshot ordinari pur con ping stabile. Il traffico voluminoso mantiene il controllo della congestione. I campi di fade degli effetti visivi vengono inizializzati e azzerati per evitare checkpoint non validi.

## Avvio senza PowerShell

La versione 7 aggiunge **Riconnettiti** e **Immagine nitida** in **Co-op online**.
Le prove sono in `docs/qa_menu_blur_2026-10-04.md` dentro `source.zip`.
Riconnettiti riprova l'ultima sessione con lo stesso ruolo e indirizzo: il client
torna nell'host ancora aperto; un host senza mappa riapre la lobby e sceglie poi
Continua salvataggio. Non usa gli eventuali nuovi valori digitati nei campi;
per un altro IP usare Connetti.

Immagine nitida è attiva di default su ciascun PC e toglie dalla visualizzazione
scie ImageTrail e blur radiale, anche durante l'intro. Gli script e gli effetti
continuano ad aggiornarsi. Disattivarla restituisce gli effetti originali;
non modifica le preferenze grafiche della partita singola.

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

I datagrammi ENet partono con un limite di **1200 byte**. Se, dopo il caricamento del client, mancano ACK per tre secondi o lo stesso comando del mondo resta senza conferma per cinque secondi, l'host riapre il peer e negozia **576 byte** alla riconnessione automatica. Il listener rimane aperto e la campagna resta in pausa fino all'applicazione del mondo. Il limite viene modificato soltanto su una connessione nuova, per evitare frammenti già accodati troppo grandi. Questa è una modalità di recupero, non una misura della MTU effettiva di Radmin o Hamachi. Per aggiornare, estrarre il nuovo ZIP in una cartella nuova e avviare il launcher da lì **su entrambi i PC**; conservare profili e salvataggi. Diagnosi e prove in `docs/qa_blocco_remoto_2026-10-04.md` dentro `source.zip`.

## Salvataggi, journal e recupero

L'host salva campagna e stato di entrambi sotto `Documenti\Amnesia\AmnesiaOnlineCoop_Host\online`. Il launcher grafico usa questa cartella su ciascun PC, anche come client, per permettere il cambio di autorità. I vecchi profili dello script restano nella propria sottocartella: selezionarli con **Change Profile** per i rispettivi salvataggi.

Per una partita chiusa normalmente, ricollegarsi dalla lobby, premere Pronto su entrambi e far scegliere Continua all'host. Il journal trasferisce note, ordine dei diari e obiettivi attivi/completati anche al join e al caricamento, senza riaprire automaticamente documenti già raccolti.

**Esci** e **Salva ed esci** terminano la sessione volontariamente e riportano
al menu principale: non riaprono la mappa. Salva ed esci esegue l'autosalvataggio
se sei l'host; il client non salva una copia locale incompleta del mondo.
Se esce il client, l'host tiene la campagna in pausa e aspetta che si ricolleghi,
senza migrazione automatica. Se esce l'host, il client torna al menu principale.
Le cadute involontarie conservano il recupero automatico già implementato.
Aggiornare entrambi i PC: anche il protocollo 10 distingue questi casi.

La perdita di connessione mette la campagna in pausa e avvia tentativi automatici di riconnessione. Se l'host resta assente e il client ha un checkpoint completo e validato, il client può diventare host dopo circa 12 secondi. Ripristina il salvataggio nativo, inverte correttamente i giocatori, esegue un autosalvataggio e **aspetta il partner**. Non prosegue da solo.

I checkpoint sono catturati ogni 20 secondi, oltre al primo stato completo e ai cambi mappa. Si possono perdere i progressi successivi all'ultimo checkpoint ricevuto, con ritardo aggiuntivo dovuto al trasferimento. Il vecchio host ancora aperto prova a raggiungere il nuovo automaticamente. Dopo un crash e riavvio, collegarsi all'IP del nuovo host con lo stesso profilo per conservare l'identità. Non eliminare configurazioni/profili durante la sessione. La migrazione richiede checkpoint valido e raggiungibilità in entrambe le direzioni; non è senza pausa.

Inventario e progressione sono condivisi. Nelle nuove campagne online entrambi partono con olio al 100%, anche nel primo livello che originalmente azzera quello di Daniel. Olio e attivazione restano individuali e l'olio si consuma normalmente. Caricamento, cambio livello e riconnessione conservano l'olio salvato. La lanterna richiede ancora di raccogliere l'oggetto nella campagna. Il client aggiorna anche l'adattamento originale degli occhi al buio, separatamente dalla luce della lanterna. Il partner vede luce e posa dell'avatar; non è incluso un modello dettagliato della lanterna in terza persona. I nemici possono scegliere entrambi, inseguono un bersaglio per volta e verificano separatamente la visibilità. Nascondersi rompe la visuale, ma possono cercare nell'ultima posizione conosciuta.

Il protocollo 10 trasferisce le posizioni dei prop in movimento a 20 Hz senza attendere le ritrasmissioni del mondo e interpola i corpi sul client su 50 ms. Le posizioni vecchie vengono scartate; la posizione finale, creazione, distruzione e cambi di proprietà restano affidabili. Porte e casse restano simulate dall'host: l'interazione conserva il tempo di andata e ritorno della rete. Aggiornare entrambi i PC; protocollo 9 e 10 non sono compatibili.

## Limiti ancora aperti

- Campagna completa, puzzle, inseguimenti, finali e Hard da verificare su due PC. Le prove coprono integrazioni e alcuni livelli, non un'intera partita.
- Prediction locale limitata al movimento orizzontale e a 500 ms di copertura dei gap, con collisioni e correzione dell'host. Gli input locali restano immediati. Salti, crouch e oggetti fisici restano autorevoli; reti lente possono ancora causare correzioni visibili.
- Migrazione provata tra processi locali; crash del PC, riapertura del gioco, partizioni prolungate e WAN richiedono ulteriori prove.
- La regressione con 100 ms nominali e perdita periodica del 5%, che prima cadeva durante il cambio livello, passa con il nuovo controllo delle code. Questo test locale non certifica tutte le reti: perdita prolungata e ritrasmissioni possono ancora produrre intervalli e correzioni visibili. Un ping stabile non garantisce un intervallo massimo tra gli stati.
- Alcuni effetti script, creazione/trasformazione delle luci e tipi di entità fuori dal roster richiedono copertura. Audio e particelle replicano stato e ciclo di vita, non ogni campione sonoro o il seme casuale degli emettitori.

Gli errori sono nel `hpl.log` della cartella del profilo. Se non si collega, controllare IP, porta, firewall, build e dati. La console sviluppatore conserva `net_status` e `net_ping`, ma non serve per iniziare una partita.

I nuovi messaggi di connessione riportano `[t=...ms]` dal contatore monotono del singolo PC, la dimensione negoziata dei datagrammi, il codice ricevuto alla disconnessione e il tempo dall'ultimo pacchetto applicativo. I contatori dei due PC non hanno la stessa origine. `[NET/DIAG] Transport service paused` segnala un intervallo superiore a 250 ms senza servizio della rete sul thread del gioco, possibile anche durante un caricamento: da solo non identifica la causa. `[NET/STATS]` registra RTT, variazione e code del trasporto; `[NET/PERF]` distingue replica e checkpoint. Se ritardo o blocchi persistono, conservare il `hpl.log` di entrambi subito dopo la partita e prima di riaprire il gioco, che può sovrascriverlo. Il launcher usa `Documenti\Amnesia\AmnesiaOnlineCoop_Host\hpl.log` anche per il client.
