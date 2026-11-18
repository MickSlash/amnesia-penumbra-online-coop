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
