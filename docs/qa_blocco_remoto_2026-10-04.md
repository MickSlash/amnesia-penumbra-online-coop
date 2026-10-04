# Blocco remoto della baseline e regressione del recupero

## Evidenza reale

Segnalazione successiva alla build del protocollo 8: i giocatori non si vedono,
il client rimane fermo. Il log del client è stato incollato nella conversazione;
il log dell'host è stato letto da Documenti e conservato nel workspace in
`build/remote-startup-regression/host-user-20261004.log` prima dei test.

Il client carica Rainy Hall in 817 ms, con altri 160 ms circa di precaricamenti.
La pausa del trasporto è 1014 ms. L'RTT iniziale è 48–50 ms. Riceve il primo
snapshot e poi non riceve più dati applicativi né conferme di trasporto per
decine di secondi. Non applica la baseline del mondo.

L'host conferma MapReady all'epoch 1, ma WorldApplied rimane zero. I dati
affidabili in volo passano da 36676 a 57832 byte; le copie periodiche delle pose
continuano ad accumularsi. Il costo della replica durante questa fase è circa
230–244 microsecondi medi, con massimo osservato 2554 microsecondi. Non emerge
un sovraccarico del thread della replica che spieghi questo blocco.

Dopo 30 secondi il controllo applicativo dell'host chiama Stop: elimina il
listener e il compagno, poi permette alla campagna locale di proseguire.
Il client continua a tentare handshake verso un host che non ascolta più.
La condivisione della telecamera nasconde gli avatar anche prima del termine
della baseline, rendendo invisibile il partner durante l'attesa bloccata.

Sono difetti verificabili della build 8. La causa esatta della mancata consegna
dei datagrammi sui due PC rimane da verificare: dai soli log non si distingue
un limite del tunnel da un errore Winsock/provider o da altri filtri di rete.
L'RTT di 447 ms stampato dopo il blocco è l'ultima stima, non una misura di una
connessione che sta continuando a scambiare correttamente i dati.

## Modifiche del protocollo 9

- Il timeout di caricamento appartiene alla sessione, con limite di 45 secondi.
  Resetta il peer senza chiudere il listener o cambiare ruolo. Il recupero tiene
  ferma la campagna; non libera gli script per proseguire da soli.
- Dopo MapReady, tre secondi senza ACK o cinque secondi con lo stesso
  comando voluminoso ancora privo di ACK attivano un tentativo con MTU
  negoziata di 576 byte. Gli ACK delle altre informazioni non nascondono
  una lacuna permanente; normali ritrasmissioni brevi non provocano un reset.
  Il vecchio peer viene azzerato prima: i frammenti accodati non vengono mai
  mantenuti dopo una riduzione della MTU. L'identità di riconnessione resta
  verificata. Se anche il percorso conservativo fallisce, rimane il recupero
  con attesa limitata, senza dichiarare la partita sincronizzata.
- Una sola copia affidabile della posa può essere in attesa di ACK. Gli
  snapshot ordinari a 60 Hz e le copie unsequenced continuano a portare lo
  stato più recente; i pacchetti mancanti non creano una coda di vecchi movimenti.
- Il budget ordinario di dati affidabili in attesa scende da 32 a 8 KiB.
  Le porzioni dei checkpoint scendono da 16 a 1 KiB, mantenendo controllo di
  integrità, ordine, validazione del salvataggio e conferma applicativa.
  Un pacchetto ritrasmesso non sospende più tutto il nuovo traffico del mondo:
  il budget limita l'accumulo. Il backoff dei comandi in volo è limitato da
  una scadenza adattiva fra 200 e 1000 ms; scadenze più brevi già calcolate
  da ENet rimangono valide. Si evita che la stima RTT gonfiata dal caricamento
  lasci un frammento senza nuovo tentativo per molti secondi.
  La soglia dei tentativi ENet usa uno shift unsigned e un controllo esplicito
  oltre 31 tentativi: i retry più frequenti non possono causare shift indefiniti.
- Su Windows i buffer ENet vengono raccolti in un unico datagramma prima di
  WSASendTo, con limite controllato di 4096 byte. È un irrobustimento del percorso
  Winsock, non la dimostrazione di un difetto nel driver del PC remoto.
  Il risultato negativo di enet_host_service viene ora registrato con il codice
  del sistema operativo, la dimensione e il numero di buffer. I log aggiungono
  contatori dei datagrammi inviati e ricevuti.
- Gli avatar vengono nascosti e la telecamera condivisa soltanto dopo la
  conferma della baseline. Formato dei salvataggi invariato; entrambi devono
  aggiornare perché protocollo e dimensione massima dei chunk cambiano.

## Verifica del trasporto

La suite compilata con AddressSanitizer supera le regressioni esistenti e nuove:
codec malformati, input/seriali/epoch, journal, checkpoint, identità, lobby,
riconnessione e inversione dei ruoli. Include le pause sincrone di 6,5 secondi
di host/client, la conferma applicativa, una lacuna nel canale affidabile e
il mantenimento degli snapshot senza creare transizioni di vita duplicate.
Log: `build/remote-startup-regression/asan-tests.log`.

La scadenza effettiva del caricamento viene verificata avanzando di 45001 ms
l'orologio ENet nel solo processo del test: il ramo di timeout conserva il
listener e la stessa identità si riconnette con generazione incrementata.

La nuova prova di invio raccoglie 65 buffer in un datagramma di 4096 byte,
verifica ogni byte ricevuto e rifiuta 4097 byte senza scrivere fuori dal buffer.
Un relay C++ scarta tutti i datagrammi sopra 900 byte: la baseline iniziale
rimane bloccata, il listener sopravvive al reset, la stessa identità si
riconnette, la nuova baseline usa 576 byte e WorldApplied raggiunge l'host.

Con il binario precedente, la prova nativa Rainy Hall su relay da 900 byte
fallisce: manca Complete host entity baseline sent. Log e contatori conservati
in `build/remote-startup-regression/legacy-mtu900*`; 124 datagrammi sopra soglia
su 2135 ricevuti. Questa riproduzione copre un guasto di consegna per dimensione,
non tutte le caratteristiche del guasto remoto: nel relay continuano ad arrivare
alcune conferme piccole, mentre nei log reali cessano anche quelle.

## Prova intermedia del recupero e dell'intro

Binario intermedio, SHA-256
`CE879903CD68857673F4611AA592FB30775E0C102C3B91901F5D19E7A96AD26C`.

`RunCampaignStartTest.ps1 -MaxDatagramBytes 576 -ExpectMtuRecovery` parte
dalla lobby nativa, sceglie Nuova campagna ed esegue i timer e le callback
originali senza saltare l'intro. Due processi locali con profili nuovi,
35 ms di ritardo per direzione, un datagramma ogni 20 scartato e una pausa
client aggiuntiva di 6 secondi a ogni caricamento della replica.

Questo caso passa in `build/campaign-start/20261004_160602_42c07f05` e
`build/remote-startup-regression/intermediate-intro-mtu576.log`, ma il binario
non è quello finale:

- Il trasferimento iniziale perde i datagrammi troppo grandi. Un solo reset
  automatico ricostruisce la baseline all'epoch 2 con limite 576 byte.
- I due ruoli hanno esattamente due handshake, conferma del mondo e
  checkpoint completi. Nessun errore di socket o rifiuto di schema.
- Su entrambi partono i quattro stream CH01L00_DanielsMind01_01 … 04_01
  e sono presenti i sottotitoli. La prova controlla i canali audio nativi
  e il testo renderizzabile, non registra l'uscita fisica degli altoparlanti.
- Il risveglio originale rilascia i controlli; gli input ordinari producono
  1,900 m di movimento host e 2,931 m client. Entrambi terminano senza errori.
- Relay: 23190 ricevuti, 22023 inoltrati, 1167 scartati, di cui 8 sopra
  soglia. Datagramma massimo inoltrato 576 byte, iniziale massimo 1166.

Il test iniziale della riduzione del budget ha rilevato chunk checkpoint
da 16 KiB che non rientravano più nella coda da 8 KiB. La correzione usa
chunk da 1 KiB; la suite completa viene ripetuta con questo formato.
La prova nativa intermedia è stata interrotta prima di ricompilare;
non è conteggiata tra le prove riuscite del binario finale.

La successiva prova intermedia con soglia 1200 fallisce l'asserzione di una
sola connessione: compaiono tre handshake. Conservata in
`build/campaign-start/20261004_160928_fbcea681` e
`build/remote-startup-regression/intermediate-intro-normal-failed.log`.
Il primo fallback derivava dal blocco globale per qualsiasi ritrasmissione;
una successiva caduta avveniva pur con dati applicativi recenti e comandi
affidabili dal backoff troppo lungo. Le modifiche successive eliminano
il blocco globale, limitano le scadenze e misurano il singolo comando fermo.
L'asserzione non viene allentata per far passare il caso normale.

## Verifiche prima della protezione del contatore

Binario Windows x64 compilato con MSVC v145, SHA-256
`811D56D8288575A27E9035719AAF28991CA5480B9C7A138D712F2A5EA8B16F39`.

PASS con soglia 1200 byte, 70 ms nominali di RTT, perdita periodica del 5%
e pausa client aggiuntiva di 6 secondi. Artefatti in
`build/campaign-start/20261004_161724_b888b8d4` e
`build/remote-startup-regression/pre-guard-intro-normal.log`.

Su ciascun ruolo: esattamente un handshake, nessuna disconnessione o fallback,
baseline applicata, checkpoint completo, quattro voci originali con sottotitoli
e uscita pulita. Dopo il risveglio gli input ordinari producono 2,495 m di
movimento host e 2,931 m client. Relay: 24999 ricevuti, 23750 inoltrati,
1249 scartati, nessuno sopra soglia; datagramma massimo 1200 byte.

Lo stesso binario supera il caso con soglia 576 byte, mantenendo 70 ms
nominali, 5% di perdita e 6 secondi aggiuntivi di caricamento client.
Artefatti in `build/campaign-start/20261004_162050_fc1d045b` e
`build/remote-startup-regression/pre-guard-intro-mtu576.log`.
Esattamente due handshake per ruolo: uno iniziale e uno dopo il recupero.
Baseline, conferma del mondo, checkpoint, quattro dialoghi e sottotitoli
risultano presenti su entrambi. Dopo il risveglio: 2,391 m host e 2,931 m client.
Uscite pulite, nessun errore del socket o rifiuto. Relay: 31506 ricevuti,
29851 inoltrati, 1655 scartati, di cui 88 sopra soglia; massimo iniziale
1196 byte, massimo inoltrato 576 byte.

## Regressione del gameplay prima della protezione

Stesso hash del binario, RTT nominale 100 ms, perdita periodica del 5%,
limite datagrammi 1200 byte. `RunImpairedNetworkTest.ps1 -Media -Prediction
-SnapshotBlackout` supera i controlli di pickup e inventario condiviso,
prediction con collisioni, blackout di tre secondi dei soli snapshot ordinari,
audio/particelle/musica/effetti, lanterne indipendenti e permessi locali obsoleti,
salvataggio e caricamento, cambio Entrance Hall → Old Archives, morte e ripresa
ordinate di entrambi. Nessun rifiuto della replica.

Log: `build/remote-startup-regression/pre-guard-media.log`, `pre-guard-media-host.log` e
`pre-guard-media-client.log`. Relay: 6788 ricevuti, 6449 inoltrati, 339 scartati,
nessuno sopra soglia; massimo 1200 byte. PASS con uscita pulita di entrambi.
Il passaggio tra mappe in questo harness è richiesto dalla fixture di test,
non dimostra che i giocatori abbiano risolto i puzzle o finito il livello.

## Verifiche del binario da distribuire

La revisione finale aggiunge soltanto la protezione della soglia ENet oltre
31 retry. Hash dell'eseguibile:
`1CF9ECBE34E7336FC3133F6655E361919E3EAE1060B5EA0450F6EAC2D45DFE80`.

La suite ASAN completa passa, compreso il nuovo test che forza un comando
affidabile a 40 tentativi e verifica il normale evento di disconnessione del
peer scaduto. Le regressioni di baseline, MTU, timeout, journal, checkpoint,
identità e riconnessione rimangono attive.

La ripetizione dell'intro originale normale passa su questo binario:
`build/campaign-start/20261004_163110_983f04ed` e
`build/remote-startup-regression/native-intro-normal.log`.
Un handshake per ruolo, quattro voci con sottotitoli, baseline e checkpoint,
uscite pulite. Dopo il risveglio: 1,776 m host e 2,931 m client.
Relay da 70 ms nominali e 5% di perdita: 24761 ricevuti, 23523 inoltrati,
1238 scartati, nessuno sopra soglia; massimo 1200 byte.

Le ripetizioni sullo stesso binario finale passano anche per:

- Gameplay/media/prediction/blackout: tutti i controlli descritti sopra,
  100 ms nominali e 5% di perdita. Log `native-media.log`, `media-host.log`,
  `media-client.log` nella cartella `build/remote-startup-regression`.
  Relay: 6944 ricevuti, 6597 inoltrati, 347 scartati, massimo 1200 byte.
- Rainy Hall MapOnly con soglia 576 byte: due handshake per ruolo, un solo
  recupero, baseline applicata all'epoch 2 e checkpoint valido sul client.
  Questo test carica direttamente la mappa e non prova l'intro originale.
  Log `native-mtu576-map.log`, `mtu576-host.log`, `mtu576-client.log`.
  Relay: 11213 ricevuti, 10558 inoltrati, 655 scartati, di cui 96 sopra
  soglia; massimo inoltrato 576 byte, iniziale 1196 byte.
- Migrazione host, cronologia journal, parità della serializzazione nativa,
  worker checkpoint ritardato di 350 ms e lavoro diventato obsoleto:
  PASS e uscite pulite. Log `native-recovery.log` e `recovery-final`.

Tutti i percorsi di log in questa lista sono relativi a
`build/remote-startup-regression`. Sono prove automatiche separate con
profili nuovi; nessun salvataggio dell'utente viene utilizzato o sovrascritto.

## Limiti

I test usano processi locali e i dati Steam installati, con profili di prova
separati. Non certificano Radmin sui due PC reali, la variante DVD, una sessione
di ore o l'intera campagna. Conservare entrambi i log dopo la prossima prova
per distinguere fallback, errore del socket e mancata applicazione del mondo.
