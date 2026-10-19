# Server dedicato Amnesia su Linux: configurazione e stato

Aggiornamento: 7 ottobre 2026.

Un server dedicato per questa mod e' fattibile, ma richiede modifiche al motore e
al multiplayer. La scelta proposta e' C++, per riutilizzare HPL2, Newton, IA,
AngelScript e i salvataggi nativi. La configurazione in `server/` e' implementata
e validabile; **il runtime dedicato headless non e' ancora implementato**.
L'eseguibile Lux attuale non legge questo file e continua a ospitare un
giocatore locale e un client remoto. Non e' ancora disponibile un binario Linux
con cui ospitare la campagna su una VPS senza grafica.

## Perche' avrebbe senso

Il processo della VPS sarebbe l'autorita' della campagna: entrambi i giocatori
sarebbero client. La sessione non dipenderebbe dal focus della finestra, dalle
prestazioni grafiche o dall'uscita dal gioco del giocatore host. Lo stato della
campagna e i salvataggi resterebbero sul server. Una disconnessione di un
giocatore potrebbe mettere in pausa la campagna senza migrare l'host.

Il server non eliminerebbe la latenza: conta il tragitto di ciascun giocatore
verso la VPS. Anche i client continueranno ad aver bisogno di prediction,
interpolazione e correzioni delle interazioni fisiche. Una VPS congestionata puo'
introdurre jitter. I caricamenti delle mappe e i costi della simulazione vanno
misurati; non e' stato verificato alcun requisito minimo di CPU o RAM.

C++ consente di conservare la simulazione originale. Rust puo' essere utile per
un supervisore o servizi amministrativi, ma integrarlo con questo motore C++
richiederebbe un binding o un wrapper; le chiamate al motore conserverebbero i
suoi vincoli di memoria e thread. Non risolverebbe automaticamente i crash
dell'engine. Riferimento: [Rust Nomicon, FFI](https://doc.rust-lang.org/nomicon/ffi.html).

## Configurazione preparata

File: `server/config/linux-vps.ini`. E' il contratto proposto per il futuro
runtime. Gli intervalli sono policy di validazione, non valori gia' supportati
dal gioco attuale.

| Parametro | Valore iniziale | Significato previsto |
| --- | --- | --- |
| Endpoint | `0.0.0.0:27020`, UDP IPv4 | Ascolto sulle interfacce della VPS |
| Giocatori | 2 | Entrambi remoti; ulteriori giocatori fuori dallo scopo iniziale |
| Simulazione | 60 Hz | Un passo ogni circa 16,7 ms, se il server tiene il ritmo |
| Snapshot giocatori | 60 Hz | Frequenza massima nominale; non e' una garanzia del tempo di consegna |
| Snapshot mondo | 20 Hz | Porte, casse e altre entita'; gli eventi critici restano separati |
| Datagramma massimo | 1200 byte | Budget previsto; lo scheduler dovra' rispettarlo |
| Dati originali | `/opt/amnesia/game` | Installazione Linux originale, necessaria per mappe e risorse |
| Prima mappa | `maps/main/ch01/00_rainy_hall.map` | Nuova campagna; il runtime dovra' usare gli start point originali |
| Salvataggi | `/var/lib/amnesia-coop/saves` | Stato della campagna e di entrambi i giocatori |
| Autosave | 60 secondi | Richiesta periodica, oltre ai checkpoint su cambio mappa |
| Ripresa al riavvio | `true` | Riprendere solo un salvataggio completo e compatibile |
| Pausa senza giocatori | `true` | Fermare la simulazione, mantenere attivo il trasporto |
| Log | `/var/log/amnesia-coop` | Diagnostica del futuro processo dedicato |

La frequenza degli snapshot non puo' superare quella della simulazione e deve
dividerla. Ridurre l'intervallo nominale fra gli stati non puo' compensare una
coda di rete bloccata o un client che non elabora i pacchetti.

Il parser C++14 rifiuta chiavi/sezioni sconosciute o duplicate, parametri
mancanti, numeri fuori intervallo, indirizzi non IPv4 e percorsi ambigui.
Controlla anche che dati, salvataggi e log non siano sovrapposti come percorsi
testuali. Non risolve symlink, non controlla l'esistenza dei file, non crea
cartelle e non verifica i permessi della VPS. Non ci sono espansioni di variabili,
include o comandi shell nel formato. I commenti sono righe che iniziano con
`#` o `;`; non sono ammessi commenti in coda a un valore.

## Compilare e verificare la configurazione

Questi comandi compilano **solo il validatore e i test**, senza HPL2 e senza
avviare una partita. Richiedono un compilatore C++14 e CMake 3.16 o successivo.
Si eseguono dalla radice del repository su Linux:

```sh
cmake -S server -B build/dedicated-config -DCMAKE_BUILD_TYPE=Release
cmake --build build/dedicated-config --parallel
(cd build/dedicated-config && ctest --output-on-failure)
./build/dedicated-config/AmnesiaDedicatedConfig --validate server/config/linux-vps.ini
```

Il validatore restituisce 0 per una configurazione valida, 1 per un errore di
configurazione o lettura e 2 per argomenti non validi. Non apre la porta UDP.
Il suo messaggio finale precisa che manca ancora il runtime headless.

Su Windows, usare CMake con il generatore Visual Studio disponibile e
`--config Release` al build; per CTest aggiungere `-C Release`.

## Modifiche necessarie prima dell'avvio su VPS

1. **Avvio Linux e backend headless.** `LuxBase.cpp` crea oggi il motore con
   `CreateHPLEngine(eHplAPI_OpenGL, eHplSetup_All, ...)`. `Engine.cpp` inizializza
   grafica, audio e GUI. I flag di setup esistenti non costituiscono un percorso
   headless completo. Occorrono un target server e backend senza finestra,
   contesto OpenGL e dispositivo OpenAL. Mesh, collisioni, animazioni, luci
   rilevanti per l'IA, camera virtuale, durata dei suoni e script devono continuare
   a funzionare. Nascondere una finestra o usare Xvfb sarebbe un percorso
   provvisorio con dipendenze grafiche, non la soluzione finale.
2. **Due client remoti.** `NetSession` ha un unico `peer_`, crea un host ENet con
   capacita' 1 e assegna sempre il giocatore remoto all'ID 2. `LuxNetwork` associa
   il giocatore 1 al processo host. Servono due slot con identita', input,
   acknowledgment, timeout e stato di caricamento indipendenti. Le snapshot
   verso ogni client devono confermare i suoi input, non quelli dell'altro.
   Aumentare solo la capacita' ENet non basta. Serve una versione di protocollo
   compatibile con il nuovo modello; il protocollo attuale rimane 11.
3. **Trasporto indipendente dai caricamenti.** Un thread deve possedere tutte le
   chiamate ENet e scambiare code limitate con la simulazione. Caricamento mappe,
   compressione salvataggi e asset non devono fermare keepalive e ricezione.
   Le snapshot obsolete possono essere sostituite; eventi affidabili e comandi
   di campagna devono preservare ordine e integrita'. Nessuna chiamata concorrente
   non protetta al motore o alla sessione ENet.
4. **Campagna e presentazione per entrambi.** La barriera delle mappe deve
   attendere entrambi i client senza fermare il trasporto. Intro, flashback,
   messaggi, journal, audio, luci/lanterne e sblocco dei controlli devono essere
   replicati anche al giocatore 1. IA, porte, oggetti e puzzle rimangono autorevoli
   sul server. Entrambi i client devono poter comandare il proprio personaggio.
5. **Salvataggio e ciclo di vita.** Salvare entrambi i personaggi e gli script
   senza dipendere dal menu del giocatore host. Scritture atomiche, controllo
   versione e ultimo checkpoint valido prima della ripresa. Gestire la pausa
   quando manca un partner, la riconnessione con la stessa identita', il riavvio
   e l'arresto ordinato. La migrazione host attuale va adattata o disabilitata
   per il dedicato, evitando due autorita' sulla stessa campagna.
6. **Accesso e installazione.** Prima di esporre il servizio su Internet servono
   un accesso riservato alla sessione e limiti alle code per peer. Il target
   Linux dovra' verificare i dati e i permessi all'avvio e consentire l'esecuzione
   con utente dedicato senza privilegi root. Il pacchetto non deve distribuire
   gli asset del gioco. Solo dopo queste implementazioni avra' senso fornire
   un servizio systemd che esegue un binario server realmente disponibile.

## Verifica svolta e criteri per chiamarlo giocabile

Il validatore e i test sono compilati con MSVC v145, C++14, su Windows x64.
CTest ha superato entrambi i test (parser e CLI): 55 verifiche del parser sul
file Linux proposto, con casi validi e rifiuto di input errati. Sono stati
verificati anche i codici di uscita per file mancante (1) e uso errato (2).
Il motore del gioco,
le prestazioni del futuro server e le connessioni di due PC a una VPS non fanno
parte di questi test. Ubuntu WSL e' registrato su questo PC ma non si avvia:
il percorso del disco `ext4.vhdx` non e' disponibile. La compilazione Linux
resta quindi da verificare; nessuna distribuzione o dipendenza e' stata installata.

Per certificare il primo MVP dedicato occorre almeno:

- Avvio su Linux senza display/GPU/dispositivo audio, senza Xvfb, con due client
  remoti e controllo effettivo di entrambi.
- Nuova campagna, intro completa, sblocco dei controlli e passaggio Rainy Hall ->
  Old Archives, con le stesse entita', puzzle, journal e progressi sui due client.
- Movimento, salti e scale sotto latenza e perdita simulate, verificando anche
  porte/casse, lanterna visibile, audio e nemici che interagiscono con entrambi.
- Caricamento a cache fredda, salvataggio e ripristino dopo riavvio; un client
  che si disconnette e rientra; assenza di crescita illimitata delle code.
- Misure del tempo per tick, del divario fra snapshot e delle correzioni sui
  client. Per 60 Hz un tick ha un budget di circa 16,7 ms; i percentili e i picchi
  vanno misurati sul server reale. Nessuna promessa di fluidita' assoluta basata
  sul solo ping.

Questa configurazione e' il primo elemento verificato della preparazione del
dedicato. Non certifica la campagna e non sostituisce la build giocabile attuale.
