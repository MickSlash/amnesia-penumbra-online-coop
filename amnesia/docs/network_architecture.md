# Amnesia Online Co-op — architettura e stato

## QA successiva

La [verifica estesa del 2 ottobre](qa_2026-10-02.md) aggiunge screenshot di livelli
originali, prova del Grunt, stress ASAN e rete con ritardo/perdita. Ha trovato
consegna intermittente dello stato di morte e differenze di cutscene/fog/HUD.
Il passaggio delle prove precedenti non certifica la robustezza di questi casi.
Le [correzioni del 3 ottobre](qa_fixes_2026-10-03.md) separano morte/checkpoint
dal movimento e aggiungono input analogico delle interazioni e presentazione.

## Stato implementato

La branch `feature/online-coop` contiene una prima integrazione giocabile per due
processi: l'host simula entrambi i giocatori, Newton, nemici, interazioni e script;
il client invia intenti e visualizza il mondo autorevole a schermo intero.
La campagna completa non è ancora certificata: le prove automatiche coprono
Entrance Hall e Old Archives, non una partita dall'inizio ai titoli di coda.

Implementati:

- Snapshot a 60 Hz di posizione, orientamento, altezza occhi, salute, sanità,
  olio, acciarini, lanterna, crouch, morte e stato attivo dei due giocatori.
  Buffer client da 100 ms, interpolazione e correzione dei teletrasporti.
  Orientamento della camera locale immediato; prediction orizzontale limitata a 500 ms, con replay degli input non confermati e collisioni Newton.
- Camera canonica con roll, FOV, moltiplicatore aspect, offset della testa e
  sensibilità script. LookAt e interazioni che consumano il mouse mantengono la
  visuale canonica. Il normale orientamento non applica due volte sensibilità/sway.
- Transizioni affidabili di morte, respawn e attivazione, campionate ogni tick;
  coda distinta dagli snapshot di movimento. Il client conserva morte e rinascita
  anche se arrivano nello stesso Poll; fade minimo di 400 ms e snap al checkpoint.
- Replica di props, aree e nemici tramite ID host e generazione della mappa,
  trasformazioni dei corpi, animazioni, attivazione, distruzione, luci dei props,
  porte bloccate/chiuse e creazione dinamica dagli asset installati.
- Intenti affidabili di interazione, acciarino, attacco, lanterna, uso e combinazione degli
  oggetti. L'host riusa raycast, distanza e stati originali: il client non sceglie
  un bersaglio né invia trasformazioni da applicare al mondo.
- Inventario condiviso autorevole e uso di oggetti nel contesto di P2.
- Cambio livello e ricaricamento del salvataggio con barriera Map/Ready,
  baseline completa del mondo e isolamento degli eventi fra mappe.
- Salvataggi appartenenti all'host con stato di P2; morte e checkpoint condivisi.
- Replica affidabile dei nuovi eventi di voce, messaggio, nota e diario;
  delta di colore, raggio, visibilità e attivazione delle luci esistenti.
- Hint, fade, image trail, sepia, radial blur e flash degli script; fog globale
  iniziale e modifiche di colore, intervallo, esponente, attivazione e culling.
- Launcher grafico AmnesiaCoop.exe, menu online nativo, lobby Pronto, journal completo, riconnessione automatica e migrazione da checkpoint nativo.

## Autorità e integrazione del motore

`network/NetSession` non dipende da Lux: gestisce ENet, handshake, code e codec.
`LuxNetwork` è un modulo globale aggiornato anche nei menu. L'input remoto è
applicato prima di `LuxMapHandler::UpdatePlayer2`, senza registrare P2 come secondo
modulo e senza abilitare split-screen. Lo spawn prova la shape Newton vicino a P1.

Sul client P1 interno rappresenta il giocatore di rete 2 e P2 interno l'avatar
dell'host, ID 1. La simulazione Newton, gli aggiornamenti autorevoli di mappa e
player e l'esecuzione degli script mappa/globale/inventario sono esclusi. Camera,
mani, lanterna, avatar e presentazione continuano ad aggiornarsi.
Il mondo online resta attivo durante inventario e journal tramite il modulo
globale, conservando un solo aggiornamento della simulazione.

Le aree collegate a "Player" scattano per il primo giocatore che entra. Fanno
eccezione i trigger che negli script originali chiudono il passaggio alle spalle
del giocatore: `CoopCallbackNeedsAllPlayers` (`LuxTypes.cpp`) li elenca per mappa
e nome e li fa scattare solo quando entrambi li hanno attraversati almeno una
volta. Oggi contiene `07_archives_cellar`/`PastFirstGate` (grata della leva);
`RunImpairedNetworkTest.ps1 -Gate` ne verifica il comportamento.

L'host resta proprietario degli script e degli effetti sul gameplay. Il client
non esegue una seconda copia di AngelScript. Gli eventi di presentazione coprono
solo i tipi elencati: questo non implica che ogni funzione script della campagna
abbia già una replica. Una disconnessione conserva il mondo e P2, mette la campagna in pausa e avvia il recupero. Il client riprova lo stesso endpoint con identità persistente. Un checkpoint completo e validato consente di trasferire autorità e invertire i giocatori; il nuovo host aspetta il partner.

## Protocollo (versione 18)

Questa sezione è nata con il protocollo 7; dove il codice è cambiato fa fede
`amnesia/src/game/network/NetSession.h`, che documenta ogni versione. Le ultime:

| Versione | Cambiamento |
| --- | --- |
| 12 | I nemici inviano il movimento non affidabile come gli oggetti fisici. |
| 13 | Pulsanti dell'inclinazione nell'input del client; scale specchiate delle entità. |
| 14 | Flag 128 delle entità: porta a battente rotta. |
| 15 | Nessun cambio sul filo; rifiuta host con il difetto dei percorsi delle immagini degli oggetti. |
| 16 | Nessun cambio sul filo; rifiuta host che inviano percorsi assoluti degli autori originali. |
| 17 | Un comando di movimento per tick logico; ogni pacchetto (`InputBatch`) ripete gli ultimi 6 comandi e l'host ne simula uno per tick (`InputQueue`), con un buffer adattivo contro il jitter e il recupero degli arretrati. | Nella 0.6.0 lo snapshot passa a 151 byte: per giocatore i rapporti della velocità massima con e senza corsa, lo stato di corsa e il flag 128 (scala a pioli o arrampicata, senza previsione). `cameraX`/`cameraZ`/`eyeHeight` non contengono più il ritardo della media della telecamera (`CameraSmoothPosNum`): il client rifà la media sui piedi mostrati. Nuova azione 8 (rotellina). |
| 18 | Lo snapshot porta l'inclinazione (Q/E) separata (`leanMove`, `leanRoll`): il client calcola la propria con `cLuxPlayerLean` e sostituisce quella dell'host. Il salto è rieseguito dal corpo del motore insieme al movimento (spinta, gravità ridotta per `MaxJumpCount`), senza la stima verticale separata. Le azioni si legano al comando già spedito nel tick. |
| 19 | Messaggio `LoadScreen` sul canale 0: fra due mappe l'host manda categoria, voce della citazione e immagine della propria schermata di caricamento; il client la mostra (nella propria lingua) e carica la mappa sotto di essa. L'host tiene la schermata finché il client non ha applicato il nuovo mondo, poi manda la chiusura e la dissolvenza parte su entrambi. Le luci con lo stesso nome nella stessa mappa si identificano con `nome#n`. |

### Inviti Steam

Con Steam attivo l'host apre una lobby riservata agli amici e un ponte
(`network/MessageBridge`): i datagrammi ENet di ogni utente della lobby passano
per `ISteamNetworkingMessages` (relay di Steam) e arrivano alla porta ENet
dell'host da un socket locale dedicato. Il client che accetta l'invito apre il
ponte inverso e collega ENet a `127.0.0.1`. ENet, il protocollo e l'identità di
ripresa non cambiano, quindi il rientro in una partita in corso usa lo stesso
recupero del collegamento per IP. Il ponte gira su un proprio thread e funziona
anche durante i caricamenti. Con Steam la migrazione dell'host è disattivata
(l'indirizzo del compagno è il ponte locale). Le prove usano
`AMNESIA_NET_TEST_STEAM_FAKE`, che sostituisce Steam con porte UDP locali.



ENet 1.3.18 incluso da sorgenti, licenza MIT conservata; mod GPLv3. Header AO,
versione uint16 big-endian e tipo uint8; interi espliciti, nessuna struttura C++
trasmessa in memoria. Quattro canali:

| Canale | Affidabilità | Contenuto |
| --- | --- | --- |
| 0 | Affidabile e ordinato | Hello/Welcome, Map/MapReady, LoadScreen, Action, Peer lobby, CheckpointAck, Snapshot di riserva a 10 Hz e al cambio lanterna |
| 1 | Ordinato, non affidabile | InputBatch a ogni tick logico, 60 Hz (gli ultimi 1–6 comandi, 10 + 21 byte per comando), Snapshot a 60 Hz (159 byte) |
| 2 | Affidabile e ordinato | Entity, WorldReady, Inventory, Presentation, Life (159 byte), Media (115–627 byte), Journal |
| 3 | Affidabile e ordinato | Checkpoint compresso, frammenti da 1 KiB, massimo un invio per tick a 60 Hz |

Ogni stato gameplay include un'epoch di mappa. Sequenze obsolete o duplicate
sono ignorate con aritmetica che supporta il wraparound. MapReady abilita snapshot
e intenti; WorldReady e applicazione della baseline abilitano l'input client.

Gli snapshot di riserva hanno lo stesso codec da 159 byte e lo stesso spazio di
sequenza degli snapshot ordinari. Il client accetta solo lo stato più recente,
indipendentemente dal canale: un backup ritardato non sovrascrive una posa nuova.
Il canale 0 li separa dalle code affidabili di entità/media sul canale 2. Entità,
audio e luci mantengono una frequenza separata di 20 Hz. La coda ENet di ricezione
ha un limite di 4 MiB, con singolo pacchetto applicativo limitato a 64 KiB:
baseline e frammenti fuori ordine possono superare 256 KiB e necessitano spazio
per la ritrasmissione mancante che consente di svuotare la coda.
Non entrano nella coda Life e non cancellano prediction o interpolazione.
Il protocollo 6 evita connessioni con client precedenti che rifiutavano Snapshot
sul canale 0. Life conserva la semantica di morte/ripresa e la coda da 64 elementi.

Entità, media, presentazione e checkpoint vengono rinviati se i byte affidabili
in coda/in volo, incluso il nuovo messaggio, superano 32 KiB o se un pacchetto
attende un ACK da oltre 250 ms oppure è in ritrasmissione. I chiamanti conservano
lo stato da inviare e riprovano; i delta del mondo/media si aggiornano all'ultimo
stato disponibile. Gli snapshot di riserva, Life e controllo restano inviabili.
`bulk_paused` mostra questa pressione nei log. Inventario e journal conservano
i propri limiti e la consegna affidabile della baseline completa.

La sessione configura il throttle ENet con decelerazione RTT pari a zero: picchi
degli ACK durante checkpoint/caricamenti non devono ridurre a zero l'invio di
input e snapshot non affidabili. I limiti di banda ENet restano rispettati;
frequenze fisse e `BulkReady` regolano il traffico applicativo voluminoso.
Entità iniziali assenti dal roster host vengono rimosse dal client.
Un'entità del roster che non può essere applicata chiude la replica, senza
dichiarare completata una baseline incompleta. Life ha al massimo 64 elementi;
overflow, cambio mappa e disconnessione eliminano la coda. La baseline attende
anche l'applicazione della coda Media (massimo 1024 messaggi). Il roster Media
host è limitato a 2048 elementi e invia al massimo 64 cambiamenti per tick.
I vecchi percorsi assoluti di authoring degli asset originali già caricati da HPL
sono convertiti nel nome logico locale usato dal fallback del motore; i pacchetti
continuano a rifiutare percorsi assoluti e traversal.

Media distingue SoundEntity, ParticleSystem e priorità musicali. Un ID distinto
dal nome evita collisioni tra effetti omonimi; gli oggetti runtime ricevono un
UniqueID conservato dal salvataggio HPL. Il client applica creazione, posizione,
proprietà e rimozione. I media originali della mappa assenti dal roster host sono
rimossi dopo la baseline. La musica riceve priorità, loop, volume, fade e posizione
temporale solo ai cambiamenti di stato, senza seek a ogni tick. Il client non
decide autonomamente la musica di pericolo/inseguimento.

Presentation include anche suoni GUI degli script, fade dei SoundEntity,
volume/velocità globale degli script, EmotionFlash e ScreenShake. Volume e
velocità globali vengono inoltre trasferiti nello stato iniziale. Si mantengono
le impostazioni audio personali del client. La velocità del fade HPL, che può
essere negativa, viene trasmessa in valore assoluto: la destinazione ne determina
il verso. Audio e particelle usano asset locali; fase esatta delle varianti SNT,
seme casuale ed età interna degli emettitori non sono sincronizzati.

PlayerInput contiene anche delta yaw/pitch del mouse e il tasto Rotate. Il client
accumula i delta fra i tick di invio; l'host li consuma una volta per sequenza
per porte, leve, ruote, slide e rotazione di oggetti. Durante queste interazioni
non trasforma l'errore di camera in una forza ripetuta. Inversione e Rotate
provengono dal client, senza leggere i controlli fisici dell'host per P2.

Handshake massimo 5 s, timeout ENet fino a 10 s, intenti di movimento scaduti
in 250 ms, caricamento client massimo 30 s. Poll non bloccante limitato a 64 eventi;
code limitate e pacchetti ENet massimi di 64 KiB. Codec controllano dimensioni,
range, tipi, stringhe, percorsi e canale/direzione. Gli asset restano locali.
La fingerprint FNV-1a di `.map` e `.hps` verifica compatibilità di quei due file;
non è una verifica crittografica né copre tutti gli asset, configurazioni e script
inclusi. Entrambi i PC devono usare la stessa versione della build e dei dati.

Il roster viene inviato con massimo 64 cambiamenti per tick. I corpi del mondo
usano delta affidabili, senza interpolazione fisica client; sotto perdita elevata
il backlog può aumentare. Le code piene chiudono la sessione invece di consentire
una crescita illimitata. Questo comportamento va ancora misurato su WAN.

## Verifica e limiti

Release x64 compilata con MSVC v145. I test di protocollo verificano endian,
troncamenti, byte aggiunti, range, percorsi, sequenze, timeout input, isolamento
fra mappe, inventario, baseline ed eventi della storia. I test UDP in processi
separati verificano handshake, rifiuto della versione errata e disconnessione.

`RunGameSmoke.ps1 -Campaign` avvia due Lux.exe con profili di prova separati e
asset dell'installazione legittima. Controlla movimento P2 e camera client,
baseline, raccolta di un acciarino una sola volta, consumo di una pozione con
salute aggiornata, inventario client, luci e messaggio, salvataggio/ricaricamento
con stato remoto conservato, cambio Entrance Hall → Old Archives e morte di P2
con ritorno di entrambi al checkpoint. È una prova automatica delle integrazioni,
non una verifica visiva o un attraversamento manuale dei puzzle della campagna.
Voci e documenti hanno codec e hook implementati, ma richiedono prova manuale
con contenuti reali e apertura/chiusura del journal.

Prima di dichiarare un MVP dell'intera campagna servono:

1. Partita completa su due PC, con audit dei trigger, puzzle, inseguimenti,
   teletrasporti, cutscene e finali. Verifica separata della modalità Hard.
2. Copertura degli effetti script ancora mancanti: effetti speciali,
   suoni diretti fuori dagli hook coperti, trasformazioni/creazione di luci e tipi di entità
   non compresi nel roster attuale (per esempio rope/commentary).
3. Prove su due PC e WAN, riavvio dopo crash, partizioni prolungate e progressi recuperati da checkpoint; la migrazione può perdere lo stato successivo al checkpoint ricevuto.
4. Misure di banda e fluidità su due PC, prediction verticale e interpolazione degli oggetti fisici se necessarie. Il client prevede solo movimento orizzontale, fino a 500 ms.
5. Verifica manuale di grab, porte, leve, combinazioni, voci/documenti e nemici, oltre ai casi automatizzati. NAT traversal, relay, matchmaking e discovery non sono inclusi.

Non commettere binari né dati proprietari. Build e configurazioni generate sono
ignorate da Git; il launcher grafico usa AmnesiaOnlineCoop_Host su ciascun PC.

`RunGameSmoke.ps1 -Media` estende la regressione della campagna con suono ed
emettitore persistenti, ricaricamento, modifica e rimozione, musica e presentazione
audio/effetti. Richiede sia il timer smoke sia `AMNESIA_NET_TEST_MEDIA`; il launcher
normale elimina entrambe le variabili. La prova enemy include un probe temporaneo
con corpi Newton: controlla entrambi visibili, uno nascosto alla volta con cambio
bersaglio, entrambi nascosti. Il probe allarga range/FOV e poi ripristina lo stato;
non è una prova di fuga completa, sensibilità al rumore o tutti gli incontri.

L'host salva la campagna e lo stato P2. Inventario condiviso, olio e attivazione
lanterna individuali; il client ricostruisce le due luci con ownership invertita.
La luce e la posa dell'avatar remoto sono visibili, senza una lanterna dettagliata
in terza persona. La selezione bersaglio del nemico valuta entrambi ogni 0,75 s;
un nemico sceglie un bersaglio alla volta e mantiene l'ultima posizione conosciuta
durante la caccia. Gli attacchi di shape possono danneggiare entrambi i corpi se
sono nell'area del colpo. Oscurità, immobilità, accovacciamento e lanterna
modificano il raggio di rilevamento separatamente per ciascun giocatore.

## Journal, prediction e recupero

La chiusura volontaria usa il codice ENet `0x414f0001`. Il ruolo locale diventa
subito Offline; Poll continua la consegna affidabile del disconnect in background
per al massimo tre secondi, senza attesa sul thread. Il peer distingue l'uscita
dal timeout: host in pausa sullo stesso listener quando esce il client, ritorno
al menu del client quando esce l'host. La migrazione vale per le cadute involontarie.
Il reset dei comandi Esci/Salva ed esci chiude prima la sessione; i salvataggi
host vengono catturati prima di cessare l'autorità. Riconnettiti usa ruolo e
endpoint precedenti con l'identità di ripresa conservata.

Immagine nitida è una preferenza di presentazione online, attiva di default:
alla fine di EffectHandler::Update disattiva soltanto il rendering di ImageTrail
e RadialBlur. Non cambia Disabled, parametri, fade, script o configurazione
grafica originale. Disattivandola ripristina gli effetti dai parametri correnti.

Journal trasferisce fino a 512 voci e 60000 byte con revisione e map epoch. Note,
diari ordinati per gruppo e obiettivi attivi/completati provengono dalla collezione
host; la sostituzione client attende la chiusura del journal per non invalidare
puntatori delle pagine aperte. Il caricamento e ogni nuova connessione inviano una
baseline completa, indipendente dagli eventi recenti di presentazione.

Gli snapshot includono velocità e parametri del CharacterBody. La prediction del
giocatore locale rimuove gli input confermati dal tick effettivamente applicato
sull'host e ripete quelli pendenti: massimo 32 comandi e 500 ms. Questo è un limite
di copertura dei gap, non un buffer che ritarda gli input. Verifica la shape
Newton contro il mondo, consente slide orizzontali e disabilita il replay durante
salti/crouch, morte e look controllato dagli script o dagli oggetti. Il partner
rimane interpolato con buffer di 100 ms. Non è rollback dell'intera fisica.

Le piccole correzioni orizzontali della prediction locale si riassorbono con
decadimento esponenziale di 80 ms; gli input nuovi restano immediati. La posizione
visualizzata viene verificata contro le collisioni. Scarti oltre 25 cm, cambi
verticali oltre 25 cm, morte, crouch e controllo script/oggetti cancellano questa
correzione. Le transizioni Life e i cambi mappa cancellano anche la prediction.
`Player state intervals` registra media e massimo degli arrivi effettivamente
applicati, per finestre di circa cinque secondi: RTT e frequenza degli arrivi
sono misure distinte.

ImageTrail, Sepia e RadialBlur inizializzano e azzerano la velocità di fade anche
prima del primo effetto. I checkpoint non serializzano così valori indeterminati
che il validatore potrebbe correttamente rifiutare come non finiti.

Hello/Welcome e Peer scambiano identità di ripresa, porta e stato Pronto. La
configurazione conserva l'identità locale; la sessione ammette il precedente
partner dopo una disconnessione e dopo inversione dei ruoli. L'identità evita
riassegnazioni accidentali dello slot: non costituisce autenticazione crittografica.
La perdita di input per oltre tre secondi sospende la simulazione (prima era un secondo: ogni picco di latenza congelava il mondo per entrambi); fra 250 ms e 3 s il P2 dell'host resta fermo. Pause, ripresa e buchi di input oltre 500 ms sono registrati come `[NET/DIAG]`; la nuova map
epoch riavvia questa verifica dopo il primo input, evitando un'attesa circolare
tra baseline e input. Il mondo e P2 sono conservati durante il recupero.

I checkpoint includono il salvataggio nativo completo e la collezione delle mappe
visitate. Sono compressi con zlib, limitati a 32 MiB espansi e trasferiti sul
canale 3 a circa 60 KiB/s prima degli overhead. Il checksum FNV64 verifica la
completezza; non è una firma. Il client valida struttura XML, tipi, factory,
lunghezze degli array, componenti dei vettori, valori finiti e profondità prima
della deserializzazione. Conferma il checkpoint solo dopo questa verifica.

Dopo un'assenza prolungata il precedente host cede l'autorità (circa 8 secondi),
il client con un checkpoint valido tenta la promozione (circa 12 secondi), scambia
P1/P2 nel salvataggio e attende il partner. Sono timer del recupero dopo il
rilevamento della disconnessione, non garanzie di tempo dalla caduta del cavo.
Un checkpoint ogni 60 secondi (20 nelle prove QA) più il trasferimento comporta
possibile rollback. Se l'RTT supera il minimo recente di max(40 ms, metà del
minimo), l'invio scende da 60 a 4 KiB/s per non accodare gli snapshot.
Raggiungibilità bidirezionale, identità conservate e checkpoint valido sono
necessari. Crash/restart dei PC e partizioni WAN non sono certificati.

`RunRecoveryGameTest.ps1` interrompe il trasporto dopo ACK di un vero checkpoint.
Verifica due secondi di interruzione e, separatamente, inversione host/client,
variabile campagna, salute individuale e cronologia journal. Esercita inoltre
rifiuto di classi sconosciute, vettori incompleti e factory incompatibili.
