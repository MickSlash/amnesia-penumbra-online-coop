# Penumbra: Overture co-op — analisi di rubber banding, desync e jitter

17 novembre 2026. Analisi del codice di rete esistente (`game/CoopNetwork.*`, `game/network/*`) e del motore HPL1 incluso nel repository, più misure su due istanze locali. Nessun sistema è stato riscritto: sono stati aggiunti solo strumenti di misura.

Legenda usata in tutto il documento:

- **MISURATO**: numero letto dai log `[COOP/NETDIAG]` delle prove descritte in §2.
- **VERIFICATO NEL CODICE**: comportamento dimostrato leggendo il codice (file e righe indicati), non ancora quantificato in gioco.
- **IPOTESI**: spiegazione plausibile, non ancora dimostrata.

REA non è stato usato: il sorgente completo di HPL1 e del codice di gioco di Overture è nel repository, quindi l'analisi binaria non aggiungeva informazioni. Servirà per Black Plague e Requiem, il cui codice di gioco non è nel repository.

## 1. Sintesi

Il rubber banding del client ha **tre cause principali**, in ordine di peso misurato:

1. **L'host conserva un solo input del client e conferma (ACK) il più recente ricevuto, non quello effettivamente simulato.** Quando gli input arrivano a gruppi (jitter della rete, perdita di pacchetti, un frame lento su uno dei due PC), alcuni comandi non vengono mai simulati singolarmente e altri tick riusano un input vecchio. La previsione del client presume invece una corrispondenza 1:1 fra comandi e tick. Con 40 ms di jitter il 39 % dei tick dell'host riusa un input già visto, e le correzioni oltre 10 cm passano da 1–2 a 23–47 ogni 5 secondi.
2. **La previsione usa un modello cinematico scritto a mano, non il movimento nativo del motore.** Il modello sbaglia in diagonale e con comandi parziali (verificato nel codice). Anche in locale, senza rete, il 54 % delle pose della camminata richiede una correzione oltre 2 cm. *Correzione del 18/11 (§7):* gran parte di quelle correzioni non viene dal modello. Nella prova il corpo dell'host resta bloccato dai frammenti delle casse e da un muro, mentre il client prevede di attraversarli (causa C4).
3. **Stati non previsti.** Accovacciato, durante il salto, trascinando oggetti o porte (`ePlayerState_Move`) e da morto il client non prevede: resta alla posizione in ritardo dell'host oppure si muove liberamente. Alla ripresa della previsione arriva una correzione di tutto l'errore accumulato.

A queste si aggiungono **discrepanze fisiche** fra le due simulazioni (oggetti statici sul client, nemici senza collisione sul client) e **interpolazioni senza buffer** per partner, oggetti e nemici, che con arrivi irregolari si muovono a scatti.

Il tick logico è a passo fisso, 60 Hz su entrambi i PC, quindi **non c'è deriva fra gli orologi** (MISURATO: `command_drift_total` resta fra −2 e +2 per tutta la prova). Il problema non è la quantità di byte: un input pesa 30 byte e uno snapshot dei giocatori 123 byte.

## 2. Strumentazione e prove

### Diagnostica aggiunta

Ogni 5 secondi entrambi i ruoli scrivono una riga `[COOP/NETDIAG]` in `hpl.log`. È sempre attiva e non influenza le decisioni del netcode.

| Campo | Ruolo | Significato |
| --- | --- | --- |
| `rtt_ms`, `rtt_var_ms` | entrambi | RTT ENet e varianza |
| `ticks`, `burst_ticks`, `tick_gap_ms_*` | entrambi | tick logici; un tick iniziato entro 4 ms dal precedente è un recupero dopo un frame lento |
| `new_input_ticks`, `reused_input_ticks`, `expired_input_ticks` | host | tick con un input nuovo, con l'input del tick precedente, senza input (scaduto dopo 250 ms) |
| `skipped_inputs` | host | comandi del client sovrascritti prima che un tick li leggesse |
| `command_drift`, `command_drift_total` | host | comandi confermati meno tick simulati con input: misura la deriva fra i due clock |
| `snapshots`, `snapshot_gap_ms_*` | client | snapshot nuovi applicati e intervalli d'arrivo (jitter) |
| `correction_cm_*`, `poses_over_2cm`, `poses_over_10cm`, `resets_over_25cm`, `hard_over_2m` | client | errore di previsione misurato a ogni nuova posa dell'host e numero di scatti |
| `pending_*`, `ack_lag_*` | client | comandi non ancora confermati |
| `unpredicted_ticks`, `replay_truncated` | client | tick senza previsione; replay interrotti da salto, accovacciamento o dal limite di 500 ms |
| `replay_steps`, `replay_blocked` | client | passi del replay e quanti sono stati bloccati dal test di collisione |
| `host_path_m`, `shown_path_m`, `host_step_cm_*` | client | strada percorsa dal corpo autorevole e da quello mostrato; passo dell'host per posa |

File: `game/network/NetDiagnostics.h` (nuovo), `game/CoopNetwork.cpp` (`ReportDiagnostics`, agganci in `UpdateHost` e `UpdateClient`), `game/network/Prediction.h` (`TakePoseCorrection`, `LastReplayTruncated`), `NetSession::PingVariance()`.

Strumenti di prova aggiunti, attivi solo nei test:

- `tests/network/UdpRelay.cs`: opzione `jitter`, ritardo casuale uniforme per datagramma con possibile riordino (seme fisso).
- `RunNativeTest.ps1 -JitterMs`.
- `-Hitch host|client`: blocca quel ruolo 100 ms una volta al secondo durante la camminata.
- `-Walk`: intensità del comando di camminata; 1000 equivale alla tastiera.

### Prove

Prova `hammer` di `RunNativeTest.ps1`: due processi sullo stesso PC, mappa `level01_02_mine_entrance`. Il client cammina 4 s avanti e 4 s indietro. Ogni scenario è stato eseguito una volta, quindi i valori sono indicativi e non medie statistiche. Le finestre riportate sono le due della camminata.

| Scenario | RTT ms | Tick host con input riusato | Pose > 2 cm | Pose > 10 cm | Scatti > 25 cm | Correzione media / max |
| --- | --- | --- | --- | --- | --- | --- |
| A — locale, tastiera | 21–23 | 0–2 % | 129–166 / 300 | 1–2 | 0 | 2,7 cm / 11 cm |
| C — relay 35 ms, 5 % perdita | 123–127 | 13 % | 104–175 / 300 | 16–18 | 0 | 2,8–3,4 cm / 15 cm |
| D — relay 35 ms + jitter 0–40 ms, 5 % perdita | 157 | 36–40 % | 48–140 / 180 | 23–47 | 2 | 3,3–5,8 cm / 30 cm |
| E — locale, frame lento sul client | 23–27 | 5–12 % | 114–149 / 300 | 12 | 1–2 | 3,0–3,3 cm / 35 cm |
| F — locale, frame lento sull'host | 22–26 | 10 % | 99–155 / 300 | 4–12 | 1–2 | 2,9–3,1 cm / 40 cm |

Nota: in ogni prova compare anche una correzione di 157 cm. È un artefatto della prova (a 39,5 s lo script teletrasporta il giocatore, `RunHammerQA`) ed è esclusa dalla tabella.

Rapporto fra la strada mostrata sul client e quella reale dell'host, nella finestra di camminata senza il teletrasporto della prova (MISURATO): A 0,8; D **4,3** (host 2,7 m, client 11,5 m). Una strada mostrata più lunga di quella reale significa avanti e indietro continui: è il rubber banding visto dal giocatore.

Il controllo di fluidità della prova (`max_horizontal_step_m < 0.2`) fallisce in tutti gli scenari con `-Walk 1000`. Con il valore storico 0.5 una prova è passata (scenario B) e una no (passo massimo 21,5 cm): la camminata è più lenta e le correzioni restano vicine alla soglia.

## 3. Cause, con prove

### C1 — Un solo slot per l'input e ACK del più recente (VERIFICATO NEL CODICE + MISURATO) — priorità massima

- `Session::Receive` sovrascrive `input_` a ogni input più recente (`network/NetSession.cpp`, ramo `DecodeInput`). `ReadInput` restituisce sempre l'ultimo, finché non scade dopo 250 ms.
- `UpdateHost` legge un solo input per tick, lo applica con `MoveForwards/MoveSideways(dt)` e imposta `lastInput=input.sequence`. Lo snapshot invia `acknowledgedInput=lastInput` (`CoopNetwork.cpp`, `UpdateHost`).
- Il client, in `MovementPrediction::Reconcile`, scarta tutti i comandi con sequenza ≤ ACK e rigioca solo i successivi. Presume quindi che ognuno dei comandi confermati sia stato simulato.

Effetti:

1. Se arrivano 2–3 input nello stesso tick, l'ACK salta di 2–3 ma l'host ha simulato un solo tick. Nei tick successivi, senza input nuovi, l'host rigioca l'ultimo. Sul lungo periodo i tempi si compensano (deriva ≈ 0, MISURATO), ma **ogni singola posa** è sbagliata di uno o più tick di movimento, in avanti o indietro. Il client vibra.
2. Gli edge di salto, accovacciamento e corsa e i delta di mouse (`lookYaw/lookPitch`, usati in `ePlayerState_Move` e `WeaponMelee`) contenuti negli input sovrascritti vanno persi. Un salto premuto per meno di un tick dell'host può non avvenire. Lo stesso schema, delta di mouse consumati una volta e sovrascritti, è un candidato per le leve e le ruote difficili da girare segnalate in Amnesia (IPOTESI, da verificare su quel codice).
3. Se il client si blocca per più di 250 ms, per esempio durante un caricamento o un autosave, l'input scade: l'host ferma il personaggio del client. I comandi inviati in quel periodo non verranno mai simulati, e alla ripresa il client torna indietro di tutta la strada prevista. In una delle prove A uno stallo di 500 ms del client ha prodotto 15 tick `expired_input_ticks` e scatti oltre 25 cm (MISURATO).

Correlazione MISURATA: la quota di tick con input riusato (0–5 % → 13 % → 39 %) cresce insieme alle correzioni oltre 10 cm (1–2 → 16–18 → 23–47 ogni 5 s).

### C2 — Il modello di previsione non è il movimento del motore (MISURATO + VERIFICATO NEL CODICE)

- `MovementPrediction` (`network/Prediction.h`) ricostruisce il movimento con accelerazione, decelerazione e velocità massima lette dallo snapshot.
- Il motore invece usa `iCharacterBody::Update` (`HPL1Engine/sources/physics/CharacterBody.cpp`), che risolve collisioni, gradini, pendii e gravità.
- MISURATO (scenario A, camminata dritta, nessuna rete): il corpo dell'host avanza di 1,80 cm per posa con deviazione standard 1,29 cm e massimo 10 cm. Il test di collisione del client blocca solo 0–6 passi su circa 1.700. Le correzioni medie di 2,7 cm corrispondono a circa un tick e mezzo di movimento.
- VERIFICATO NEL CODICE: `iCharacterBody::Move` moltiplica l'**accelerazione** per l'intensità del comando, mentre la velocità massima resta piena. La previsione invece moltiplica la **velocità massima**. Con la tastiera, intensità ±1, i due modelli coincidono. Con valori parziali, come la prova storica a 0,5 o un futuro gamepad, divergono. MISURATO nello scenario B: host 5,1 m, client 2,0 m.
- ~~IPOTESI: l'irregolarità del passo dell'host viene dal terreno.~~ **Smentita il 18/11** (§7). Il registro della posizione mostra il corpo dell'host fermo contro i frammenti delle casse e contro un muro per il 49–73 % delle pose, con velocità piena. L'irregolarità è un blocco, che il test di collisione del client non riproduce (C4).

### C3 — Stati senza previsione (VERIFICATO NEL CODICE)

In `Reconcile` e in `UpdateClient`:

- **Accovacciato** (flag 1): nessuna previsione e nessuna correzione sotto i 2 m. Il client si muove con la propria fisica locale e diverge, poi quando si rialza riceve tutta la correzione accumulata.
- **Salto** (`buttons & 3`): il replay si interrompe al primo comando di salto o accovacciamento. Per tutto l'RTT il client resta più vicino alla posizione in ritardo dell'host.
- **`ePlayerState_Move`** (flag 16: trascinare oggetti, porte, leve): `Reconcile` restituisce la posizione dell'host senza replay, quindi il client segue in ritardo di un RTT.
- **Limite di 500 ms o 32 comandi**: oltre questo RTT la previsione si ferma.
- Correzioni oltre 25 cm o con variazione verticale oltre 25 cm: scatto immediato.

### C4 — Fisica diversa fra host e client (VERIFICATO NEL CODICE)

- `ApplyEntity` rende statici i corpi degli oggetti replicati sul client (`SetMass(0)`, `SetGravity(false)`). Poi li sposta con `SetMatrix` interpolando per 1/30 s. Sull'host lo stesso corpo è dinamico e il personaggio del client lo spinge. Sul client è un muro che si teletrasporta.
- `iGameEnemy::ApplyNetworkState` e `OnSetActive` disattivano il corpo personaggio dei nemici sul client. Sul client si "passa attraverso" un nemico, sull'host no.
- Conseguenza: vicino a casse, porte che si muovono o nemici, il client prevede un percorso che l'host non può fare. Non misurato: la prova `hammer` non cammina contro oggetti.

### C5 — Interpolazioni senza buffer temporale (VERIFICATO NEL CODICE + MISURATO)

- Gli snapshot non hanno un timestamp: c'è solo una sequenza, condivisa per giunta con le entità. Il client conserva solo l'ultimo snapshot (`snapshot_`).
- Partner sul client: insegue l'ultima posizione con un filtro esponenziale (`dt*20`, `UpdateAvatar`).
- Oggetti: interpolano verso la posa più recente in 1/30 s, partendo dalla posizione mostrata (`motion`).
- Nemici: il corpo viene spostato subito (`SetPosition`), la mesh è interpolata.
- MISURATO: con jitter 0–40 ms gli snapshot distinti applicati scendono da 60/s a 36/s, con intervallo 27 ± 13 ms e massimo 83 ms. Con frame lenti si arriva a 117 ms.
- Senza un buffer che riproduca le pose in base al tempo dell'host, ogni irregolarità d'arrivo diventa movimento a scatti di partner, oggetti e nemici.

### C6 — Ordine di aggiornamento nel tick (VERIFICATO NEL CODICE, effetto secondario)

- Ordine in un tick: fisica (`cScene` → `cWorld3D::Update` → `iPhysicsWorld::Update`), poi `cCoopNetwork::Update`, poi `ButtonHandler` e `cPlayer` (`Init.cpp:568` e seguenti, `Game.cpp:265`).
- L'host applica l'input N e cattura lo snapshot **prima** che la fisica lo integri. La posizione inviata con ACK N contiene il movimento fino a N−1.
- A velocità costante l'errore si annulla fra due pose consecutive. Pesa invece nei cambi di direzione e di velocità: un tick di errore a ogni partenza e arresto.

### Verificato senza problemi

- **Deriva dei tick**: nessuna, grazie al passo fisso a 60 Hz di `cLogicTimer` (`LogicUpdateRate=60`).
- **Ordine dei pacchetti**: input e snapshot usano un canale non affidabile ordinato. Le entità usano sequenze per ID, gli stati vecchi vengono scartati.
- **Banda**: datagrammi sotto 1200 byte. Il throttle ENet ha decelerazione 0 e non taglia gli snapshot.
- **Proprietà della fisica**: è unica (host) e non ci sono doppi aggiornamenti dello stesso corpo sul client: le pose dell'host sovrascrivono sempre. Il problema è la *differenza* fra i due mondi fisici (C4), non una doppia autorità.

## 4. Piano di intervento (C++, in ordine di priorità)

Ogni passo si verifica con le stesse prove (A, C, D, E, F) e con i test di protocollo esistenti. I passi 1–3 cambiano il formato degli input: richiedono il **protocollo 4** e rifiutano le build precedenti, come già avviene.

| # | Intervento | Cause | File | Rischio |
| --- | --- | --- | --- | --- |
| 1 | **Coda di input con ridondanza.** Il client invia in ogni pacchetto gli ultimi 4–6 comandi. L'host li mette in una piccola coda ordinata e ne consuma **esattamente uno per tick**, con un buffer adattivo di 1–3 tick. Se la coda è vuota ripete l'ultimo per al massimo ~250 ms. L'ACK diventa la sequenza davvero simulata. I delta del mouse viaggiano per comando, così nessuno va perso. | C1 | `NetSession.*`, `CoopNetwork.cpp` (`UpdateHost`, `UpdateClient`), `Prediction.h`, test di protocollo | Medio: cambia il flusso dell'input dell'host |
| 2 | **ACK coerente con la fisica**: confermare il comando la cui velocità è già stata integrata (quello del tick precedente), oppure catturare lo snapshot dopo il passo fisico. | C6 | `CoopNetwork.cpp` | Basso |
| 3 | **Previsione con il movimento nativo**: rigiocare i comandi con lo stesso `iCharacterBody` del motore, su un corpo "fantasma" del client o salvando e ripristinando lo stato del corpo locale, invece del modello scritto a mano. Copre in modo naturale pendii, gradini, accovacciamento e corsa. | C2, C3 | `Prediction.h`, `CoopNetwork.cpp`, eventuale aggancio in `CharacterBody` | Medio-alto: da misurare il costo CPU (32 replay per tick) |
| 3b | Alternativa minima a 3: correggere l'intensità (accelerazione, non velocità massima) e prevedere anche corsa e accovacciamento con le velocità del corpo locale. | C2, C3 | `Prediction.h` | Basso |
| 4 | **Buffer di interpolazione temporale**: snapshot e pose delle entità con tick dell'host, riproduzione ~100 ms indietro e interpolazione fra due pose note, per partner, oggetti e nemici. Cambia il formato degli snapshot: protocollo 4 insieme al passo 1. | C5 | `NetSession.*`, `CoopNetwork.cpp` (`UpdateAvatar`, `motion`, `ApplyEntity`) | Medio |
| 5 | **Coerenza delle collisioni nella previsione**: escludere dal test di previsione gli oggetti replicati e i nemici (decide l'host), oppure renderli cinematici con velocità. Lasciare all'host gli esiti fisici. | C4 | `CoopNetwork.cpp` (`collide`, `ApplyEntity`), `GameEnemy.cpp` | Medio |
| 6 | **Correzioni più morbide**: soglia di scatto adattiva all'RTT (es. 50 cm), smorzamento più lungo per errori medi, correzione verticale separata. | C3 | `Prediction.h` | Basso |
| 7 | **Previsione durante `ePlayerState_Move`** (trascinamento). Probabilmente utile anche per le leve di Amnesia, da verificare a parte. | C3 | `CoopNetwork.cpp`, stati del giocatore | Medio |

Ordine consigliato: 1+2 insieme (il guadagno maggiore misurato), poi 3b subito e 3 se le misure lo giustificano, poi 4+5, infine 6–7.

Criteri di uscita proposti, sullo scenario D (35 ms + jitter 40 ms + 5 % perdita):

- `skipped_inputs` a 0 a regime;
- `poses_over_10cm` sotto 2 ogni 5 s;
- nessun `resets_over_25cm` in camminata libera;
- `shown_path_m / host_path_m` fra 0,9 e 1,1.

## 5. Confronto delle tre strade

| | Migliorare il netcode C++ | Modulo di rete Rust via FFI | Riscrivere componenti HPL1 in Rust |
| --- | --- | --- | --- |
| Risolve le cause trovate | **Sì**: tutte sono nell'integrazione fra rete e motore (ordine dei tick, ACK, modello di movimento, fisica diversa) | Solo in parte: coda di input, codec e buffer sì; previsione nativa, fisica e ordine dei tick restano in C++ dietro l'FFI | Solo riscrivendo anche character body e fisica, cioè il motore |
| Rischio per il gioco originale | Basso-medio, modifiche locali e misurabili | Medio: nuovo toolchain (Rust `i686-pc-windows-msvc`), confine FFI, due allocatori, debug misto | Alto: si cambia il comportamento originale di Newton e dei personaggi |
| Lavoro stimato | Passi 1–2: pochi giorni; 3–5: 1–3 settimane | +1–2 settimane solo per toolchain, build CMake, FFI e test, senza correggere nulla in più | Mesi |
| Sicurezza nel parsing di dati di rete | Già coperta da validazione e 80.000+ casi malformati nei test | Migliore per costruzione (memory safety, `cargo fuzz`) | — |
| Riuso per Black Plague / Requiem | Buono se la parte di rete resta separata dagli agganci di gioco (`Capture`/`Apply`) | Buono: crate condivisibile, ma gli agganci di gioco vanno comunque scritti in C++ per ogni titolo | Nessun vantaggio aggiuntivo |
| Compatibilità con Amnesia (HPL2) | Le stesse correzioni valgono per il port Amnesia, che usa lo stesso schema di previsione | Possibile, con un secondo binding | No |

**Raccomandazione**: migliorare il netcode C++ seguendo il piano di §4. Le cause misurate non dipendono dal linguaggio, e una riscrittura non le eliminerebbe.

Conviene però preparare il terreno per un eventuale modulo Rust in futuro:

- tenere coda di input, buffer di interpolazione e codec in file "puri" senza dipendenze dal motore (come già `Prediction.h` e `NetSession`);
- testarli con i test di protocollo.

Se più avanti servirà condividere la rete fra Overture, Black Plague e Requiem, quei file saranno il confine naturale per un crate Rust con API C, senza toccare il motore.

La riscrittura di componenti HPL1 in Rust è sconsigliata: cambierebbe il comportamento originale del gioco senza risolvere nessuna delle cause misurate.

## 6. Limiti di questa analisi

- Due processi sullo stesso PC, una prova per scenario: i valori sono indicativi. Una prova reale su due PC via Radmin resta necessaria, e la diagnostica `[COOP/NETDIAG]` serve proprio a misurarla.
- La camminata della prova è in linea retta su una sola mappa. Collisioni con oggetti e nemici (C4), accovacciamento, salto e trascinamento (C3) sono verificati nel codice ma non misurati. Servono scenari dedicati.
- Il jitter del relay è uniforme e indipendente per pacchetto. Le VPN reali hanno spesso picchi correlati, cioè raffiche.

## 7. Aggiornamento 18 novembre — protocollo 4

### Modifiche

| Passo del piano | Cosa è stato fatto | File |
| --- | --- | --- |
| 1 | Il client invia gli ultimi 6 comandi in ogni pacchetto (`InputBatch`). L'host li accoda e ne simula esattamente uno per tick, con un buffer adattivo da 1 a 4 comandi. Gli arretrati oltre il buffer vengono scartati unendo delta del mouse e salti al comando successivo. Se la coda è vuota, l'host ripete l'ultimo comando senza delta. | `game/network/InputQueue.h` (nuovo), `PlayerInput.h` (nuovo), `NetSession.*`, `CoopNetwork.cpp` |
| 2 | L'host cattura la posa del partner **prima** di applicare il comando del tick e conferma quello già integrato dalla fisica. | `CoopNetwork.cpp` (`UpdateHost`) |
| 3b | Il replay segue `iCharacterBody::Move/Update`: l'intensità scala l'accelerazione, il passo è limitato al massimo in avanti (le diagonali non sono più veloci), si usano i limiti del corpo locale (corsa, accovacciato, salute). La previsione continua da accovacciati e durante il salto. | `game/network/Prediction.h`, `CoopNetwork.cpp` (`UpdateClient`) |

Test di protocollo: tutti PASS. Quelli nuovi coprono:

- codifica del batch e 10.000 pacchetti alterati;
- ordine e ripetizione della coda, buffer adattivo, taglio degli arretrati con fusione dei delta, perdite;
- replay nativo in diagonale, con comando parziale, da accovacciati e con salto;
- comandi inviati tutti insieme su UDP reale, ognuno simulato una sola volta.

Nuova diagnostica:

- `queue_target`, `queue_starvations`, `queue_dropped`, `queue_lost`, `queue_depth_*` (host);
- `nominal_step_cm_*`, `moving_poses`, `stalled_poses`, `free_correction_cm_*` (client);
- nella prova `hammer`, il registro `[COOP/QA] Walk` con la posizione del partner sull'host.

### Misure (stesse prove di §2, una per scenario)

| Scenario | Tick host con input riusato: prima → dopo | Comandi saltati ogni 5 s: prima → dopo |
| --- | --- | --- |
| A — locale | 0–2 % → 0 | 0–5 → 0 |
| C — 35 ms, 5 % perdita | 13 % → 0–0,3 % | 36–42 → 0 |
| D — 35 ms + jitter 40 ms | 36–40 % → 0–0,3 % | 105–122 → 0 |
| E — frame lento sul client | 5–12 % → ≤ 2 % | 16–36 → ≤ 2 |
| F — frame lento sull'host | 10 % → 0–3 % | 29–30 → ≤ 9 |

Il buffer si adatta: con jitter si stabilizza a 3–4 comandi (profondità media 2–2,5, circa 40 ms in più di latenza dell'host sul movimento del client); in locale resta a 1. L'ACK medio del client passa da 2 a circa 4–5 comandi in locale, per l'ordine corretto rispetto alla fisica e il buffer. La previsione nasconde questo ritardo al giocatore.

**Correzioni del client:** nel complesso non sono calate (A: 2,2–3,4 cm di media per posa; D: fino a 5,8 cm, 46–58 pose oltre 10 cm). Il motivo, misurato, è che la camminata della prova è quasi sempre bloccata:

- `stalled_poses` 49–73 % nella prima finestra: il corpo dell'host ha velocità piena ma non si sposta;
- il registro `[COOP/QA] Walk` lo mostra fermo a z ≈ 7,8 sopra i frammenti delle casse rotte (y 0,16 → 0,24) e poi contro un ostacolo a z ≈ 9,1 camminando all'indietro;
- `replay_blocked` è vicino a zero nelle stesse finestre: il client prevede di passare.

**Nelle pose in cui l'host si muove libero** l'errore di previsione è 0,1–2,5 cm in locale e 0,5–1,3 cm con jitter (`free_correction_cm_mean`). Resta un errore di circa un tick in alcune finestre (camminata all'indietro, 2,5 cm). È da spiegare: IPOTESI, differenza di direzione fra la telecamera del client e il corpo dell'host durante la prova.

### Conseguenze sul piano

1. **Coerenza delle collisioni (passo 5) diventa la priorità successiva.** Le correzioni rimaste vengono soprattutto da ostacoli che bloccano l'host e non il client: frammenti e oggetti fisici, probabilmente anche il personaggio dell'altro giocatore. Il test di collisione del client usa `CheckShapeWorldCollision(..., abCollideCharacter=false)` e accetta piccoli ostacoli come "gradini" (`StepBlocked`), mentre il corpo nativo non sempre riesce a salirci.
2. **La prova `hammer` va affiancata da una camminata in campo libero.** Così si separano gli errori di rete da quelli di collisione. Il controllo `max_horizontal_step_m < 0.2` misura soprattutto gli urti contro i frammenti.
3. **I passi 4 (buffer di interpolazione) e 7 (trascinamento)** restano invariati.

## 8. Aggiornamento 18 novembre — previsione con il corpo nativo (coerenza delle collisioni)

### Modifiche

- **Motore** (`HPL1Engine/.../CharacterBody.*`): `iCharacterBody` ha una modalità previsione (`SetPredicting`, `GetPredictionState`, `SetPredictionState`). In questa modalità `Update` esegue lo stesso movimento (collisioni, scivolamento lungo i muri, salita graduale dei gradini, gravità) ma salta tutti gli effetti esterni:
  - callback di spinta e di gravità e `OnHitGround`;
  - telecamera ed entità;
  - aggancio a piattaforme;
  - risveglio dei corpi vicini.
- **Previsione** (`Prediction.h`): il replay è separato dal simulatore (`Replay`, `ReconcileSmoothedWith`). Il modello cinematico resta come `KinematicStepper` per i test e per i confronti.
- **Client** (`CoopNetwork.cpp`, `NativeStepper`):
  - salva lo stato del proprio corpo;
  - lo porta alla posa e alla velocità dell'host;
  - per ogni comando non confermato chiama `SetYaw`, `Move` e `Update` come l'host;
  - legge la posizione orizzontale e ripristina il corpo.

  Con `PENUMBRA_COOP_PREDICTION=kinematic` torna al modello cinematico, per i confronti.

### Misure A/B (stesso eseguibile, una prova per modo e scenario)

| Finestra | Misura | Nativa | Cinematica |
| --- | --- | --- | --- |
| A, partenza contro i frammenti (host bloccato nel 76–82 % delle pose) | pose > 2 cm | **5** | 135 |
| | correzione media | **0,64 cm** | 2,81 cm |
| | strada client / host | **1,00** | 2,47 |
| A, ritorno verso il muro | pose > 2 cm | 211 | 154 |
| | correzione media | 2,17 cm | 2,67 cm |
| | strada client / host | 0,49 | 0,82 |
| D (jitter), partenza | pose > 10 cm | **14** | 38 |
| | correzione media | **2,16 cm** | 4,32 cm |
| | strada client / host | **2,2** | 3,1 |
| D (jitter), ritorno | pose > 10 cm | **0** | 25 |
| | correzione media | **0,72 cm** | 3,22 cm |
| | strada client / host | **1,7** | 2,7 |
| Costo `UpdateClient` | locale | 0,7–1,2 ms/tick | 0,2–0,7 ms/tick |
| | jitter (circa 13 comandi in attesa) | 1,8–2,9 ms/tick | 0,3–1,1 ms/tick |

Lettura:

- Contro gli ostacoli la previsione nativa riproduce il blocco dell'host: le correzioni spariscono quasi del tutto.
- Con jitter le correzioni oltre 10 cm calano di circa l'80 %.
- Nel ritorno verso il muro, in locale, restano circa 2 cm per posa e il client mostra metà della strada dell'host. IPOTESI: è lo stesso errore residuo di un tick visto in §7; da indagare con una camminata in campo libero.

Costo: il replay nativo fa più collisioni per comando. Con RTT 150 ms costa fino a 3 ms per tick sul client. Ottimizzazioni possibili:

- non rifare il replay della posa precedente (usato solo per misurare e ammorbidire la correzione);
- fermarsi ai comandi successivi all'ultima posa nota.

### Rimane da fare per la coerenza delle collisioni

- **Il partner non ha un corpo sul client.** Sull'host il personaggio del client urta l'host (`mbCollideCharacter`), sul client no.
- **Nemici senza collisione sul client.**
- **Arretrati dopo blocchi lunghi.** Dopo un blocco di circa 1 s del client, l'host scarta i comandi oltre il buffer. Il client li aveva già mostrati e viene riportato indietro. Meglio recuperarli con 1–2 passi del corpo in più per tick.
- **Prova in campo libero**, per isolare l'errore residuo.

## 9. Aggiornamento 18 novembre — collisioni fra personaggi, recupero degli arretrati, protocollo 5

### Collisioni fra personaggi (VERIFICATO NEL CODICE + MISURATO)

La co-op chiamava `SetCollideCharacter(false)` su entrambi i giocatori a ogni cambio mappa (`AfterMapLoad`). Lo scopo era non far bloccare i giocatori a vicenda, ma quel flag fa ignorare **tutti** i personaggi: i giocatori passavano attraverso i nemici e i nemici attraverso i giocatori. Nel gioco originale il giocatore urta i nemici.

- **Motore:** nuovo gruppo di collisione per i corpi personaggio (`iPhysicsBody/iCharacterBody::SetCharacterGroup`). In `CheckShapeWorldCollision` due corpi personaggio dello stesso gruppo diverso da zero si ignorano.
- **Co-op:** i due giocatori sono nel gruppo 1, sia sull'host sia sul client. Si attraversano fra loro e urtano i nemici come nell'originale.
- **Client:** collider statici a forma di personaggio nella posizione replicata dei nemici, perché i corpi dei nemici replicati sono inattivi. Non c'è un collider per il partner, perché sull'host i giocatori non si urtano.
- **Misura della fase "partner contro partner"** della prova hammer (il client cammina contro l'host fermo): distanza minima 0,005 m sull'host e 0,008 m sul client, correzioni circa 0. Un collider del partner provato prima e scartato fermava il client a 0,69 m mentre l'host passava, con una correzione oltre 2 m.
- **Prove funzionali:** full, intro, campagna (nemici veri, danni a entrambi, morte e respawn) e campagna con ritardo passano con i giocatori che urtano i nemici.

### Recupero degli arretrati

`InputQueue::CatchUp`: con più di Target()+2 comandi in coda, l'host simula un comando in più per tick. Il comando più vecchio viene integrato subito con `iCharacterBody::Update`, il successivo dal passo fisico. Solo oltre Target()+30 (mezzo secondo) i comandi vengono scartati, unendo i delta. Nelle prove con frame lenti sul client: `queue_caught_up` 1, `queue_dropped` 0 (prima venivano scartati 2–9 comandi per finestra).

### Errore residuo nel ritorno verso il muro

Il registro delle posizioni mostra il corpo dell'host che vibra contro il muro (z 9,07 ↔ 9,09) mentre spinge all'indietro. La "strada" dell'host risulta 6 m in 5 s con il corpo quasi fermo, e il client mostra meno vibrazione. Le correzioni di circa 2 cm di quella finestra vengono da questa vibrazione, non dalla rete (IPOTESI forte, coerente con il registro).

### Protocollo 5: riproduzione sulla linea temporale dell'host

- Snapshot (127 byte) e pose delle entità portano `hostTick`, il tick logico dell'host.
- Il client (`network/Interpolation.h`):
  - un orologio di riproduzione avanza di un tick per tick e resta indietro rispetto all'ultima posa di un ritardo adattivo, pari alla dispersione di (tick client − tick host) negli ultimi 2 s più 2 tick, fra 3 e 15 tick;
  - partner, oggetti fisici e mesh dei nemici vengono interpolati fra le due pose che racchiudono quell'istante, senza estrapolazione;
  - il filtro esponenziale sulla posizione del partner è stato rimosso.
- Diagnostica: `playback_delay_ticks_*`, `playback_held`, `partner_jerk_cm_*` (variazione della velocità del partner da un tick all'altro, cioè gli scatti).
- Per i confronti: `PENUMBRA_COOP_INTERPOLATION=legacy`.

Misure A/B (prova hammer, l'host si sposta di lato durante la camminata del client; una prova per caso; `legacy` = posa più recente con il filtro esponenziale precedente):

| Scenario | Misura | Riproduzione | Legacy |
| --- | --- | --- | --- |
| D (35 ms + jitter 40 ms) | scatti del partner, media / max | **0,21 / 0,91 cm per tick** | 0,53 / 2,43 cm per tick |
| D | ritardo di riproduzione | 5,0–5,4 tick (max 7,7) | — |
| A (locale) | scatti del partner, media / max | 0,13 / 0,51 cm per tick | 0,08 / 0,24 cm per tick |
| A | ritardo di riproduzione | 3,1–3,4 tick | — |

Con jitter la riproduzione riduce gli scatti del partner di circa il 60 %. In locale il filtro precedente risulta un po' più morbido perché arrotonda anche i cambi di direzione reali dell'host; la riproduzione li mostra come sono. Nella prova A uno stallo di 500 ms ha portato il ritardo a 28 tick per pochi istanti (`playback_held` 26), poi è rientrato.

## 10. Aggiornamento 18 novembre — protocollo 6: oggetti tenuti, porte, oggetti leggeri, teletrasporti

### Modifiche

- **Stato del giocatore** (139 → 141 byte di snapshot):
  - l'oggetto preso (`Grab`), mosso col mouse (`Move`: porte, leve, cassetti) o spinto (`Push`): ID entità, indice del corpo, modo;
  - un contatore di teletrasporti (`teleport`).
- **Host:** l'oggetto in uso viene pubblicato a ogni tick invece che a 30 Hz (`PublishHeld`).
- **Client** (`ApplyHeldPrediction`):
  - l'oggetto preso viene mostrato alla stessa posizione relativa alla telecamera che ha sull'host, applicata alla telecamera attuale del client;
  - porte, leve e oggetti spinti mostrano l'ultima posa dell'host senza ritardo di riproduzione;
  - l'oggetto tenuto non blocca chi lo tiene.
- **Client:** i corpi con massa fino alla massa spingibile del personaggio (`GetMaxPushMass`) non bloccano il personaggio del client, perché sull'host vengono spinti via dal callback del corpo.
- **Teletrasporti:** l'host incrementa il contatore se il giocatore si sposta più di 1 m in un tick (camminata circa 5 cm, caduta massima 66 cm). Il client salta alla nuova posizione senza contarla come correzione, e il partner non viene interpolato attraverso il salto.
- **Correzione di un difetto:** il corpo tenuto è ora risolto per nome a ogni tick. Un puntatore conservato diventava memoria liberata quando l'oggetto veniva distrutto mentre era in mano: nella prova hammer bloccava il client nella fase finale.

### Reattività delle interazioni (prova `full`, A/B con `PENUMBRA_COOP_HELD_PREDICTION=0`)

| Gesto del client | Locale, nuovo | Locale, prima | RTT 120 ms + jitter 20 ms, nuovo | RTT 120 ms + jitter 20 ms, prima |
| --- | --- | --- | --- | --- |
| Girare la visuale tenendo la bottiglia (primo movimento visibile) | **33 ms** | 183 ms | **33 ms** | 400 ms |
| Trascinare l'anta dell'armadietto (primo movimento visibile) | **166 ms** | 233 ms | **350 ms** | 450 ms |

L'anta resta legata al giro in rete: la sua fisica gira solo sull'host. Una previsione locale delle porte, con fisica temporaneamente posseduta dal client, è in `COOP_KNOWN_ISSUES.md`.

### Prova hammer: rete separata da script e fisica

- Il controllo di fluidità falliva sempre. Le cause, misurate: i teletrasporti dello script (39,5 s e la fase finale), le correzioni oltre 2 m che ne derivavano e due passi reali da circa 0,20 m. Questi ultimi avvengono quando il corpo dell'host sale sui frammenti delle casse e viene spinto avanti in un tick: registro `[COOP/QA] Walk`, z da 7,98 a 7,78, y da 0,16 a 0,24.
- Ora:
  - i teletrasporti annunciati non contano come correzioni;
  - ogni passo del client viene valutato 20 tick dopo, contro il passo più grande dell'host nei ±20 tick, e conta solo l'eccedenza (`step_beyond_host_m`);
  - la soglia resta 0,2 m;
  - restano invariati i controlli su correzioni oltre 2 m e su intervalli fra snapshot oltre 200 ms.

### Correzioni smorzate fino a 1 m e prova hammer a più latenze

- **Misura con il nuovo controllo:** i due passi residui da 0,20 m a 0 ms (0,34 m a 40 ms, 0,54 m a 80 ms) **non** erano dell'host, il cui passo massimo nei ±20 tick era 1 mm. Erano previsioni del client che saltavano avanti quando il corpo finiva di salire sui frammenti e il replay dei comandi in attesa tornava libero. Sopra i 25 cm la correzione veniva applicata di colpo.
- **Correzione:** i teletrasporti veri sono annunciati dall'host, quindi le correzioni fino a 1 m vengono smorzate (80 ms per gli errori piccoli, fino a 160 ms per quelli da mezzo metro). Restano a scatto solo i teletrasporti annunciati e gli errori oltre 1 m o con un salto verticale oltre 25 cm. Test unitario: "a half-metre misprediction is eased".
- **Risultati** (prova `hammer`, `-Walk 1000`, una prova per riga, relay senza perdita con `-DropEvery 0`):

| Scenario | Passo oltre l'host, prima → dopo | Passo massimo mostrato | Correzioni oltre 2 m | Esito della prova |
| --- | --- | --- | --- | --- |
| RTT 0 (circa 22 ms locale) | 0,200 → **0,035 m** | 0,084 m | 0 | PASS |
| RTT 40 (circa 92 ms misurati) | 0,338 → **0,078 m** | 0,079 m | 0 | PASS |
| RTT 80 (circa 124 ms) | 0,538 → **0,074 m** | 0,118 m | 0 | PASS |
| RTT 120 (circa 157 ms) | 0,530 → **0,062 m** | 0,063 m | 0 | PASS |
| RTT 80 + jitter 30 ms + 2 % perdita | 0,576 → **0,067 m** | 0,069 m | 0 | PASS |
| RTT 120 + jitter 40 ms + 5 % perdita | 0,499 → **0,085 m** | 0,119 m | 0 | 1 FAIL su 2 (vedi sotto) |

Nota: l'RTT "misurato" include l'elaborazione delle due istanze sullo stesso PC.

- Nello scenario più duro, in una delle due esecuzioni il client ha messo a segno 4 colpi di martello su 7 sulla sua cassa entro la finestra dello script (nell'altra esecuzione 7 su 7). È in `COOP_KNOWN_ISSUES.md`.
- **Prove funzionali sulla stessa build:** full, intro, campagna e campagna con ritardo tutte PASS.

## 11. Aggiornamento 18 novembre — tutte le 25 mappe della campagna in co-op

### Nuova prova `maps`

Comando: `RunNativeTest.ps1 -Mode maps`.

- L'host carica in sequenza le 25 mappe di Overture (da `level00_01_boat_cabin` a `level01_20_base_entrance`).
- Su ognuna aspetta che il client abbia applicato il mondo, lascia replicare entità e media per 4 s e passa alla successiva.
- Passa se entrambi i ruoli completano tutte le mappe senza disconnessioni involontarie.
- Verifica caricamento, barriera e replica, **non** la soluzione degli enigmi.

### Difetto trovato e corretto: baseline bloccata su `level01_12_water_cave_tunnels`

- **Sintomo:**
  - le prime 14 mappe passavano;
  - sulla 15ª l'host restava in caricamento (`world_applied_epoch=0`);
  - dopo 45 s la barriera faceva ripartire la connessione, all'infinito.
- **Diagnostica aggiunta:**
  - `[COOP/MAP] Waiting to apply world` sul client, con cosa manca;
  - `[COOP/MAP] Baseline still pending` sull'host;
  - `[NET/WORLD]`, `[NET/INVENTORY]` e `[NET/JOURNAL]` "Cannot encode" per i dati impossibili da codificare.
- **Misura:**
  - il client aveva inventario e diario dell'epoch, ma mai `WorldReady`;
  - l'host aveva pubblicato 83 entità su 90.
- **Causa (VERIFICATA NEL CODICE):**
  - durante la baseline `PublishWorld(true)` viene richiamata a ogni tick finché tutto parte;
  - i nemici con animazioni attive risultano sempre "mossi" e venivano rispediti ogni tick sul canale affidabile;
  - questo saturava la finestra di 8 KiB (`Session::BulkReady`), e le ultime entità non partivano mai.
- **Correzione:**
  - durante la baseline un'entità già inviata non viene reinviata se non cambia stato; il movimento riprende come posa non affidabile dopo la conferma del mondo;
  - inoltre i percorsi dei file delle entità vengono normalizzati (barre, percorsi assoluti → nome del file), come in Amnesia.
- **Risultato:** 25/25 mappe pronte su host e client (le misure complete sono più sotto).

Verifica finale sulla stessa build:

- prova `maps` in locale: 25/25 su host e client, PASS;
- prova `maps` con RTT 80 ms, jitter 20 ms e 2 % di perdita: 25/25 su host e client, PASS (su una mappa la baseline ha atteso qualche secondo, ma si è completata);
- test di protocollo: PASS;
- prove native: hammer locale e con jitter, full, intro, campagna e campagna con ritardo PASS. Hammer con frame lenti sul client: FAIL in questa esecuzione, per un singolo passo del client di 0,30 m oltre quello dell'host (nelle due esecuzioni precedenti 0,046 m). Intermittente, registrato in `COOP_KNOWN_ISSUES.md`.

## 12. Aggiornamento 18 novembre — da 2 a 4 giocatori, passi 1-4 (protocollo 7)

### Modifiche

- **Protocollo 7:**
  - `Welcome` assegna l'ID del giocatore (host 1, ospiti 2..4);
  - gli snapshot hanno da 2 a 4 giocatori, ognuno con il suo ID (`players[0]` host, `players[1]` destinatario);
  - il client rifiuta snapshot e transizioni di vita indirizzati a un altro ID.
- **`HostTransport`:**
  - un socket sull'host, una `Session` per ospite;
  - assegnazione dello slot dopo `Hello`, in base all'identità;
  - `Session::Poll` diviso in `HandleEvent` e `Maintain`.
- **Host:**
  - stato per ospite in `Guest`;
  - pose di tutti catturate a inizio tick;
  - `UpdateGuest` eseguito per ogni ospite connesso;
  - epoch della mappa per ospite;
  - eventi (messaggi, dissolvenze, suoni, particelle) a tutti gli ospiti interessati;
  - vita e rinascita per qualsiasi numero di giocatori;
  - ospiti piazzati in punti liberi attorno all'host.
- **Client:** cronologia e silhouette per ogni altro ospite, riprodotte sulla linea temporale dell'host.

### Difetti trovati durante il lavoro

- **Snapshot mai inviati (prima build):** l'host compilava i `PlayerState` senza ID, `EncodeSnapshot` li rifiutava e il client restava in attesa del mondo per 45 s. Ora `Capture` riceve l'ID. Le prove `full` e `intro` hanno trovato il difetto subito.
- **Test di protocollo intermittenti:** dopo molti avvii e arresti di ENet nello stesso processo, Windows poteva lasciare `Sleep(1)` a 15,6 ms (misurato: 311 ms per 20 pause) e gli input scadevano. I test ora tengono il timer a 1 ms per tutta l'esecuzione. Riguarda solo il processo dei test.
- **Ospiti dentro l'arredo:** gli offset fissi (0,6 m) mettevano il quarto giocatore contro l'arredo della cabina. Il piazzamento ora cerca un punto libero, e la posizione storica resta la prima scelta.

### Misure

Test di protocollo: PASS. Nuove prove:
- codec con 3 e 4 giocatori;
- assegnazione dello slot e snapshot indirizzato a un altro ospite rifiutato;
- tre ospiti su un socket, con traffico separato;
- quarto ospite rifiutato;
- riuso dello slot dopo un'uscita volontaria;
- rientro con la stessa identità.

Prova `party` (`RunNativeTest.ps1 -Mode party -Players 3|4`, tutti i processi sullo stesso PC, esterno di `level01_01_outside`):

| Giocatori | Esecuzioni | Esito | Movimento degli ospiti sull'host | Snapshot | Silhouette per client |
| --- | --- | --- | --- | --- | --- |
| 3 | 2 | PASS 2/2 | 5,98–6,04 m | 3 giocatori | 2 |
| 4 | 2 | PASS 2/2 | 5,79–7,30 m | 4 giocatori | 3 |

La posizione che ogni client ha visto per sé coincide con quella dell'host entro 0,1 m.

Due giocatori, stessa build:

| Prova | Esito |
| --- | --- |
| full, intro | PASS |
| hammer | PASS (`step_beyond_host_m`=0,035) |
| hammer con RTT 80 ms, jitter 20 ms, 2 % di perdita | PASS (0,014) |
| campaign, campaign con la stessa rete degradata | PASS |
| maps | PASS, 25/25 |

### Limiti

- Nemici, attacchi ad area e checkpoint sono ancora a 2 giocatori (`COOP_KNOWN_ISSUES.md`).
- Le prove a 3-4 giocatori non passano dal relay di rete degradata.
- Non è stata fatta nessuna partita reale su più PC.

## 13. Aggiornamento 18 novembre — nemici e checkpoint per 3-4 giocatori (passi 5-6)

### Modifiche

- **Nemici:**
  - `SelectNetworkTarget` sceglie il bersaglio fra tutti i giocatori in partita, con un contatore di visibilità per ID;
  - gli attacchi ad area colpiscono tutti i giocatori in partita;
  - `BeforeObjectBreak` fa lasciare l'oggetto a qualsiasi ospite lo tenga.
- **Slot vuoti e ingresso in corsa:**
  - gli ospiti 3-4 entrano nel mondo solo se connessi; chi si connette durante una mappa entra in un punto libero vicino all'host;
  - il primo ospite resta come nella partita a due.
- **Checkpoint:**
  - salva gli ospiti 3 e 4 (`mbPlayer3`/`mPlayer3`, `mbPlayer4`/`mPlayer4`, con l'arma);
  - un ospite non ancora nel mondo riceve lo stato salvato quando entra;
  - i salvataggi del formato precedente restano validi.
- **Prova `party`**, dopo la camminata:
  - un cane compare accanto all'ultimo ospite (gli altri tornano al punto di partenza) e deve sceglierlo e ferirlo;
  - poi la salute dell'ospite viene portata a 50 e salvata nel checkpoint, quindi cambiata e l'ospite spostato;
  - il checkpoint ricaricato deve riportare salute e posizione.

### Difetti della prova corretti durante il lavoro

- **Il cane cadeva nel vuoto:** compariva dietro l'ospite, fuori dal terreno. Ora compare sul tratto appena percorso.
- **Ospiti inattivi sull'host:** gli ospiti seguono lo stato dell'host, che nella prova restava inattivo, e i nemici ignorano i giocatori inattivi. Come nella prova `hammer`, la prova ora attiva l'host.

### Misure

Prova `party` (stesso PC):

| Giocatori | Esito | Bersaglio del cane | Salute dopo il morso | Checkpoint (salute salvata 50) |
| --- | --- | --- | --- | --- |
| 2 | PASS | giocatore 2 | 71,7 | 51,6; errore di posizione 0,015 m |
| 3 | PASS 2/2 | giocatore 3 | 59,8–71,7 | 51,6; 0,004 m |
| 4 | PASS 2/2 | giocatore 4 | 58,1–69,4 | 51,6; 0,027 m |

La salute ricaricata è 51,6 invece di 50 per la rigenerazione naturale dei secondi dopo il caricamento.

Ripresa in un processo nuovo (`resume`):
- da un salvataggio del formato precedente: PASS;
- da uno nuovo: PASS.

Due giocatori, stessa build:

| Prova | Esito |
| --- | --- |
| full, intro | PASS |
| hammer | PASS (`step_beyond_host_m`=0,040) |
| hammer con RTT 80 ms, jitter 20 ms, 2 % di perdita | PASS (0,093) |
| campaign | PASS |
| campaign con la stessa rete degradata | PASS |
| maps | PASS, 25/25 |

Limiti: come in §12, nessuna prova a 3-4 giocatori con rete degradata né su più PC. L'inventario è ancora comune.

## 14. Aggiornamento 18 novembre — uscita e rientro degli ospiti 3-4, numero di giocatori nel launcher

### Modifiche

- **Launcher:** campo "Giocatori (host)", da 2 a 4.
- **Uscita di un ospite 3-4:**
  - un ospite 3-4 disconnesso esce dal mondo;
  - posizione, salute e arma restano conservate;
  - se rientra con la stessa identità nella stessa mappa, riparte da lì;
  - un ospite diverso nello stesso slot entra invece in un punto libero.
- **Crash corretto (MISURATO):** un nemico teneva come bersaglio un giocatore uscito dal mondo fino al controllo successivo, e i suoi stati leggevano il corpo distrutto. L'eccezione 0xc0000005 cadeva in `iCharacterBody::GetFeetPosition`, risolta con il file `.map`. Ora il nemico sceglie subito un altro bersaglio. La prova ha trovato il problema attraverso il proprio registro, che leggeva lo stesso corpo.
- **Prova `party`, fase di rientro:** l'host fa cadere la connessione dell'ultimo ospite, che si ricollega da solo. Verifica che il corpo esca dal mondo e che l'ospite rientri con la stessa posizione e la stessa salute.

### Misure

Prova `party` completa (camminata, nemico, checkpoint, riconnessione):

| Giocatori | Esito | Corpo tolto | Errore di posizione al rientro | Salute prima/dopo |
| --- | --- | --- | --- | --- |
| 2 | PASS | no (il primo ospite resta, come nella partita a due) | 0,000 m | 51,6 / 53,2 |
| 3 | PASS 2/2 | sì | 0,000 m | 51,6 / 53,2 |
| 4 | PASS 2/2 | sì | 0,000 m | 51,6 / 53,2 |

La salute dopo il rientro cresce di 1,6 per la rigenerazione naturale.

## 15. Aggiornamento 20 novembre — inventari per giocatore (Fase 3)

### Modifiche

- **Inventario per ospite:**
  - sull'host ogni ospite ha il proprio `cInventory`;
  - `PlayerScope` lo rende `init->mpInventory` durante le azioni dell'ospite;
  - il client riceve solo il proprio inventario. Il formato di rete non cambia.
- **Oggetti di gruppo:** torcia, glowstick, armi, taccuino e mappa vanno a tutti i giocatori, sia alla raccolta sia all'ingresso in partita. Tutto il resto resta a chi lo raccoglie.
- **Script:**
  - le callback di raccolta, uso e combinazione restano in comune;
  - `HasItem` e `RemoveItem` cercano anche negli inventari dei compagni.
- **Salvataggi:** inventari degli ospiti 2-4 nel checkpoint. Un salvataggio vecchio dà agli ospiti gli oggetti di gruppo dell'host.
- **Prova `full`:** la chiave del lucchetto passa dall'host all'ospite con `TransferItem`. Inventari registrati: host `[]`, ospite `[padlock]`.
- **Prova `campaign`:** il client controlla il checkpoint 15 s dopo il primo ricaricamento (cambio di epoca), non a 85 s dall'avvio. Le fasi dell'host durano un tempo variabile.

### Errori trovati e corretti (MISURATO)

1. **Snapshot d'inventario rifiutato dopo il ricaricamento:**
   - `LoadFromGlobal` aggiunge gli oggetti senza svuotare l'inventario;
   - l'ospite si ritrovava con torcia e martello doppi;
   - il client rifiuta i nomi duplicati ("Rejected: invalid inventory snapshot") e chiudeva la connessione.

   Correzioni:
   - `Reset()` prima del caricamento;
   - la replica salta comunque i nomi doppi.
2. **Crash dell'host (0xc0000005), intermittente:**
   - `cSavedGame::ResetGlobalData` svuotava solo l'inventario dell'host, quindi ogni salvataggio accodava oggetti e slot a `mInventory2..4`;
   - con più di 33 slot salvati, `LoadFromGlobal` leggeva oltre la fine della lista. A volte andava in crash subito (`LoadFromGlobal` ← `LoadPartner` ← `LoadCheckpoint`, risolto con il file `.map`), a volte corrompeva la memoria e il crash arrivava all'uscita, nei distruttori dei font (stack da una build RelWithDebInfo).

   Correzioni:
   - svuotamento degli inventari degli ospiti in `ResetGlobalData`;
   - controllo dei limiti in `LoadFromGlobal`.

   Dopo la correzione il checkpoint contiene 33 slot per l'host e 33 per l'ospite.

### Misure

Build Release, una macchina, istanze native su loopback. Un esito conta come PASS solo se tutti i processi passano ed escono senza crash.

| Prova | Esito | Note |
| --- | --- | --- |
| `campaign` | PASS 3/3 | nessun crash dump; inventario dell'ospite ricaricato |
| `resume` | PASS | processi nuovi dal checkpoint della `campaign` |
| `full` | PASS | passaggio della chiave all'ospite |
| `hammer` | PASS | martello e torcia anche al compagno |
| `party -Players 3` | PASS | |
| `party -Players 4` | PASS | |
| `maps` | PASS | 25/25 mappe pronte su host e client, nessuna disconnessione |
| `ctest` (ProtocolValidation) | PASS | |

Prima della correzione del crash, 3 esecuzioni `campaign` su 5 erano finite con un crash dell'host: 2 al ricaricamento, 1 all'uscita.

Non sono state provate:
- una partita reale su più PC;
- la rete degradata con 3-4 giocatori;
- l'intera campagna con oggetti consumabili divisi fra i giocatori.

## 16. Aggiornamento 21 novembre — passare un oggetto a un compagno

### Modifiche

- **Il gesto:** con un oggetto scelto dall'inventario (stato "usa oggetto")
  si mira a un compagno e si clicca.
- **Il bersaglio:** un compagno entro 2 m sulla linea di mira, più vicino
  dell'entità mirata, riceve l'oggetto (`ItemPartnerInFront`, `GiveItem`).
- **Le regole:**
  - oggetti di gruppo: solo un avviso;
  - pile: un'unità alla volta;
  - altri oggetti: interi, oppure un avviso se il compagno li ha già.
- **Gli avvisi:** sottotitoli per chi dà e per chi riceve, nella lingua del
  gioco dell'host e solo in ASCII. I testi del motore passano per `mbstowcs`
  con la locale "C", che non gestisce gli accenti.
- **La prova `full`:**
  - la chiave del lucchetto passa dall'host all'ospite con il gesto vero,
    invece di `TransferItem`;
  - l'ospite passa un antidolorifico all'host, con l'azione del client e da
    una pila di 2;
  - l'ospite prova a passare una torcia, che va rifiutata.

### Misure

Prova `full`, PASS su entrambi i ruoli:
- `Item padlock_key given by player 1 to player 2`, poi il client apre il
  lucchetto con quella chiave;
- `Guest gave one painkiller=1 (host 0->1, guest left 1) flashlight kept=1`;
- avvisi registrati: "Given to your partner", "Received" e "Your partner
  already has: Flashlight".

Suite completa dopo la modifica, tutto PASS e nessun crash dump: `full`, `hammer`, `campaign`, `resume`, `party -Players 3`, `party -Players 4`, `maps` (25/25 mappe su host e client) e `ctest`.

Non provato: il gesto fatto a mano su due PC reali.
