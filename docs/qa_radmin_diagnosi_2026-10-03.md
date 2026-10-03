# Diagnosi e correzione dopo la prova Radmin — 3 ottobre 2026

Segnalazione: connessione riuscita con circa 54 ms di ping, cadute ripetute, schermata iniziale bloccata o diversa tra i PC, apparente avanzamento premendo Esc. Il client remoto non è accessibile da questo workspace. Dopo la diagnosi iniziale l'utente ha fornito anche il suo log, conservato in `build/vpn-freeze-qa/user-client-original.log`.

## Evidenza nel log dell'host

Letto il log reale `Documenti/Amnesia/AmnesiaOnlineCoop_Host/hpl.log`, terminato alle 16:17:40 (Europe/Rome). Copia locale conservata in `build/vpn-freeze-qa/user-host-original.log`; non contiene log QA di una prova sintetica.

- 15 handshake accettati, 15 disconnessioni ENet e 15 annunci della stessa mappa `00_rainy_hall.map`.
- Nessun `Rejected`, `Client map readiness timed out` o rifiuto della fingerprint nel log di questo host.
- Ad ogni ciclo il server invia la baseline completa e cattura un checkpoint compresso di circa 99–100 KiB.
- L'invio della baseline richiede MapReady dal client: il trasporto ha superato l'handshake e il client ha dichiarato di aver caricato la mappa. Ciò non prova che abbia poi applicato tutta la baseline o mostrato l'introduzione.
- Il messaggio ENet corrente non distingue nel testo un timeout da una chiusura decisa dal peer. Un rifiuto rilevato solo sul client può quindi apparire sul server come una generica disconnessione.

Gli avvisi della cache MAP_CACHE e dei sampler grafici compaiono anche nelle prove locali riuscite. Non sono sufficienti, da soli, per attribuire loro le disconnessioni. Il log non riporta timestamp per ciascun evento: non è possibile ricavare un intervallo esatto tra le cadute.

## Riproduzione locale

Esteso `RunImpairedNetworkTest.ps1` con `StartMap` e `MapOnly`, mantenendo invariata la prova predefinita. I log sono ora raccolti dalla cartella corretta anche per le prove MapOnly.

Comando eseguito sulla build Steam già distribuita, senza modificare il binario:

```powershell
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory GAME_PATH -StartMap 00_rainy_hall.map -MapOnly -OneWayDelayMs 27 -DropEvery 1000 -Port 27044
```

Due istanze locali, 27 ms di ritardo per direzione, perdita deterministica di un datagramma ogni 1000. Relay: 8747 datagrammi ricevuti, 8739 inoltrati, 8 scartati, 3836319 byte inoltrati. Durante la finestra di circa un minuto il client ha caricato Rainy Hall, applicato baseline, journal, inventario e snapshot, e ricevuto tre checkpoint completi. Entrambi sono usciti sul timer QA. Il server osserva la disconnessione conclusiva del client prima della propria uscita; nessun ciclo di riconnessioni come nella prova reale.

Artefatti locali: `build/vpn-freeze-qa/repro-host.log`, `repro-client.log`, `repro-relay.log`.

Questa prima prova non usa Radmin, non comprende due PC e non riproduce ogni passaggio manuale della lobby. Ha mostrato che il solo ritardo di circa 54 ms, con la perdita impostata, non basta a riprodurre il guasto.

## Evidenza nel log del client ricevuto successivamente

- 15 handshake accettati e 15 disconnessioni, oltre a 259 `Rejected: handshake timeout` durante i tentativi successivi.
- Il client carica `00_rainy_hall.map` e crea l'avatar remoto, ma non applica mai la baseline completa né il primo snapshot autorevole dei giocatori.
- Nessun rifiuto della fingerprint della mappa o dei dati applicativi. Il caricamento iniziale dura circa 938 ms; quelli successivi circa 229 ms.
- Il messaggio iniziale di configurazione Justine riguarda il secondo giocatore del progetto di partenza. Da solo non prova un'incompatibilità dei dati: l'handshake e la mappa sono stati accettati.

I messaggi piccoli passano mentre il trasferimento iniziale del mondo non termina. Questo è compatibile con la perdita dei datagrammi più grandi nel percorso VPN, ma il log non misura la MTU effettiva né identifica definitivamente il motivo delle cadute. Anche i timeout dei successivi tentativi possono avere altre cause. Non pubblichiamo indirizzi o identificativi dei PC presenti nei log originali.

## Correzione del trasporto e confronto controllato

La MTU ENet predefinita del sorgente incluso è 1392 byte. Ora host e client impostano 1200 byte prima della connessione, così ENet negozia il limite e frammenta i messaggi più grandi. Il formato applicativo e il protocollo 5 restano invariati. Non è stata modificata la configurazione di Radmin, del sistema o del firewall.

Il relay QA ora può scartare ogni datagramma sopra una soglia, registrandone numero e dimensione massima. Comando di confronto (il parametro `Executable` seleziona il vecchio o il nuovo binario):

```powershell
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory GAME_PATH -Executable BINARIO -StartMap 00_rainy_hall.map -MapOnly -OneWayDelayMs 27 -DropEvery 1000 -Port 27044 -MaxDatagramBytes 1200
```

Vecchio binario conservato nel pacchetto `AmnesiaOnlineCoop_20261003_115256_e90670`, SHA-256 `EF3495327D44295FC3553431B56182747C48956408D9147799FC927E6E1B77F9`: sotto questa soglia il client non completa la baseline e attraversa ripetuti cicli di disconnessione e riconnessione. Il test fallisce sull'asserzione `Complete host entity baseline applied`. Relay: 2406 datagrammi ricevuti, 1469 inoltrati, 937 scartati, dei quali 936 sopra soglia; massimo ricevuto 1391 byte. Alcuni snapshot piccoli passano: la riproduzione copre il blocco della baseline e le cadute, non ogni dettaglio del log reale.

Prima build della correzione, SHA-256 `E3A5A2BAD63A971768364E411B52C396C3E5BA7B9FD45E09429D4E74C06FC236`: prova Rainy Hall superata, baseline completa, journal, inventario, snapshot e tre checkpoint completi sul client; entrambe le istanze escono sul timer. Un solo handshake per ruolo, nessuna riconnessione durante la prova. L'host rileva la normale uscita anticipata del client alla fine.

Misure relay della prova riuscita: 9525 datagrammi ricevuti, 9516 inoltrati, 9 scartati dalla perdita periodica, 3865993 byte inoltrati. Nessun datagramma sopra soglia, massimo ricevuto/inoltrato 1200 byte. Artefatti in `build/vpn-freeze-qa/mtu-before` e `mtu-after`.

## Focus e diagnostica

La pausa dell'engine quando la finestra perde visibilità viene ora disattivata immediatamente alla creazione di una sessione host/client riuscita, senza attendere il successivo aggiornamento del gioco. Il servizio della rete rimane sul thread del gioco: caricamenti o altri blocchi possono ancora sospenderlo temporaneamente.

I log di sessione riportano il contatore monotono ENet, la MTU negoziata, il codice di disconnessione e l'età dell'ultimo pacchetto applicativo. Intervalli senza `Poll` superiori a un secondo producono `[NET/DIAG] Transport service paused`. Nella prova Rainy Hall riuscita il caricamento iniziale produce una pausa di 1043 ms, senza causare cadute. Questi contatori non sono orologi sincronizzati tra PC; il codice zero può indicare sia uscita normale sia timeout.

## Problemi emersi nelle regressioni

La prima regressione combinata audio/prediction con 100 ms nominali di RTT, perdita periodica del 5% e soglia di 1200 byte ha fallito il controllo delle lanterne: la prova poteva segnare il comando come inviato anche quando `SendLocalAction` lo ignorava perché la baseline non era ancora pronta. Ora aspetta la baseline, l'inventario della fixture e il suo olio; il successivo autosalvataggio aspetta la conferma della lanterna sull'host, e il test conserva gli effetti fino al controllo del caricamento. Le asserzioni richieste rimangono le stesse.

Una successiva prova diretta ha fallito anche la raccolta automatica dell'oggetto, impedendo la sequenza di salvataggio/cambio livello, e ha rilevato checkpoint contenenti `cLuxHeadPosAdd.mfMoveSpeed` non valido. Nel costruttore originale velocità e distanza di rallentamento non erano inizializzate: potevano essere serializzate anche con movimento inattivo. Ora sono inizializzate a zero insieme alla destinazione. La validazione dei checkpoint continua a rifiutare NaN/Inf; non è stata indebolita per accettare salvataggi errati. Artefatti delle prove fallite: `build/vpn-freeze-qa/media-first` e `media-direct-first`.

La suite protocollo/trasporto compilata con AddressSanitizer è passata: 80000 casi codec malformati deterministici e altri 20000 casi di stress, handshake, input, seriali, epoch, code limitate, journal, identità, checkpoint, rifiuto di versione/dati errati e riconnessione. Nessun errore ASan riportato. Il test nativo di riconnessione di due secondi sulla prima build della correzione è passato, inclusi journal e ripristino della mappa. Queste prove non sostituiscono quelle sul binario finale dopo l'inizializzazione dei salvataggi.

La ripetizione sul binario finale SHA-256 `5E4CD94D103F575CBFC9C255F5ACA57C4B93B18D479A9568D36BFF3604EF89F2` supera i controlli di audio, effetti, lanterne indipendenti e salvataggio media su entrambi, oltre alla prediction e al gameplay host. La prova combinata **resta fallita** con 100 ms nominali di RTT e perdita periodica del 5%: il client si disconnette nel cambio a Old Archives, recupera con baseline e checkpoint all'epoch 4, ma non osserva la sequenza completa di morte/ripresa all'epoch 3. Non abbiamo allentato questa asserzione. Relay: 4084 ricevuti, 3880 inoltrati, 204 scartati, zero sopra soglia, massimo 1200 byte. Log in `build/vpn-freeze-qa/media-final-loss`. Non possiamo attribuire questo fallimento alla dimensione dei datagrammi: resta un problema di affidabilità da approfondire sotto queste condizioni di perdita.

## Regressioni riuscite sul binario finale

- Ripetizione Rainy Hall con soglia 1200 byte, 27 ms per direzione e perdita di uno ogni 1000: **PASS**, un handshake, baseline completa e tre checkpoint sul client, nessuna disconnessione sul client. Relay: 9442 ricevuti, 9433 inoltrati, 9 scartati; massimo 1200, zero sopra soglia. Log in `build/vpn-freeze-qa/mtu-final`.
- Stesso test combinato con ritardo di 27 ms per direzione, perdita periodica di un datagramma ogni 1000 e limite 1200: **PASS completo** su entrambi. Verifica pickup, consumo di una pozione, movimento/prediction con collisioni, salvataggio/caricamento, cambio Entrance Hall → Old Archives, morte e ripresa ordinate, suoni, particelle, musica, effetti, lanterne indipendenti e olio. Relay: 5688 ricevuti, 5683 inoltrati, 5 scartati; massimo 1200, zero sopra soglia. Nessun checkpoint rifiutato; log in `build/vpn-freeze-qa/media-final-light-loss`.
- `RunRecoveryGameTest.ps1 -Mode migrate -Port 27062`: **PASS**, due processi locali, checkpoint validato, cambio host, ripristino della variabile di campagna, inversione degli stati individuali, vecchio host ricollegato e cronologia del journal verificata. Log in `build/recovery-qa/migrate`.

La perdita configurata dal relay è deterministica e i ritardi sono nominali: non rappresentano una misura del vostro tunnel. Le prove usano finestre di circa un minuto e non certificano stabilità per ore. L'hash sopra identifica l'eseguibile testato; `manifest.json` del nuovo pacchetto permette di verificarne la corrispondenza.

## Limiti e riprova richiesta

La correzione risolve il guasto nel confronto locale con perdita per dimensione. Non abbiamo accesso a due PC con Radmin e non certifichiamo ancora la soluzione del caso reale. La sequenza completa di menu, cinematiche e sottotitoli iniziali sui vostri PC richiede una nuova prova: il test MapOnly carica il livello direttamente, senza ricreare tutti i passaggi manuali della lobby. Non è una certificazione della campagna intera.

Entrambi devono estrarre il nuovo pacchetto in una cartella nuova e avviare il launcher di quella cartella, conservando profili e salvataggi. Riprovare la stessa sessione Radmin; se si verifica ancora il blocco, conservare i nuovi `hpl.log` di entrambi prima di riavviare. Non occorre cambiare la MTU di Windows o della VPN per usare la correzione.
