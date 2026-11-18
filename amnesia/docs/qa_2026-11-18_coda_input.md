# QA 18 novembre 2026 — coda dei comandi (protocollo 17) e interpolazione dei nemici

## Problema

Verificato nel codice della 0.5.1:

- Il client mandava un pacchetto ogni due tick logici (30 Hz) con il solo ultimo input; i delta del mouse erano accumulati.
- L'host teneva solo l'ultimo input ricevuto (`NetSession.cpp`, ricezione di `Input`) e lo applicava a ogni tick finché non ne arrivava un altro.
- La previsione del client invece assegnava a ogni comando esattamente due tick.

Con il jitter un comando durava sull'host 1 o 3 tick invece di 2. Due pacchetti arrivati insieme ne perdevano uno, e un pacchetto perso perdeva il suo comando. Ogni differenza diventava una correzione della posizione del client: il rubber banding segnalato dai giocatori.

Il campo `tick` dell'input veniva trasmesso ma mai usato.

I nemici sul client venivano spostati direttamente a ogni posa ricevuta, a 20 Hz, senza interpolazione.

## Modifiche

### Protocollo 17

Stessa soluzione già misurata in Penumbra (protocollo 4).

- Il client manda un comando per tick logico, con i propri delta del mouse.
- Ogni pacchetto (`InputBatch`) ripete gli ultimi 6 comandi, con sequenza e tick. Un pacchetto perso non costa comandi, purché ne arrivi uno dei 5 successivi.
- L'host mette i comandi in una coda (`network/InputQueue.h`) e ne simula uno per tick.
  - Un buffer adattivo da 1 a 4 comandi assorbe il jitter.
  - Dopo un blocco del client gli arretrati si recuperano con un comando in più per tick: il corpo del personaggio viene fatto avanzare di un passo.
  - Solo un arretrato oltre mezzo secondo viene tagliato, conservando i delta del mouse e i salti.
- La conferma negli snapshot resta il comando già integrato dalla fisica. Nel tick dell'host la fisica gira prima della cattura dello snapshot e prima che la mappa applichi l'input successivo.

### Interpolazione dei nemici

I nemici già noti vengono interpolati dalla posa mostrata alla nuova in 50 ms, cioè l'intervallo di invio: corpo con interpolazione lineare, mesh con slerp. È lo stesso schema già usato per gli oggetti fisici. Comparsa, baseline e salti di oltre 5 m restano immediati.

### Diagnostica

- Client, `[NET/PREDICTION]`: per ogni nuova posa autorevole, quanto la previsione viene spostata (oltre 2 e 10 cm, media, massimo).
- Host, `[NET/INPUT]`: tick con comando nuovo o ripetuto, buffer, comandi persi, scartati e recuperati.
- `AMNESIA_NET_LEGACY_INPUT=1`, su entrambi i PC: invio e consumo dei comandi come nella 0.5.1, per confronti A/B nella stessa build.
- La validazione dei checkpoint ora registra il motivo e la fase del rifiuto. Tre casi prima erano silenziosi: classe di un contenitore diversa, fase di decompressione e analisi, eccezioni.

### Prova con rete degradata

`RunImpairedNetworkTest.ps1` ha `-JitterMs`: ritardo casuale aggiuntivo per pacchetto, con pacchetti che possono arrivare raggruppati o fuori ordine.

## Misure A/B

Stessa build, `RunImpairedNetworkTest.ps1 -Prediction`: il client cammina avanti con la visuale che oscilla. Due esecuzioni per caso.

| Rete | Modalità | Pose con correzione oltre 2 cm | Correzione media | Oltre 10 cm |
| --- | --- | --- | --- | --- |
| ritardo 50 ms per tratta, 5 % di perdita | 0.5.1 | 44–46 % | 1,8–2,0 cm | 0,4–0,5 % |
| | protocollo 17 | **6–9 %** | **0,4–0,7 cm** | 0,4–1,2 % |
| ritardo 40 ms, jitter 0–30 ms, 2 % di perdita | 0.5.1 | 55 % | 2,6 cm | 0,8–1,0 % |
| | protocollo 17 | **10 %** | **0,7 cm** | 0,8 % |

Le correzioni piccole, quelle che si sentono come tremolio e strappi continui, calano di circa 5 volte.

Quelle oltre 10 cm non cambiano. Vengono dal modello di previsione, che approssima il corpo del personaggio del motore; non vengono dalla consegna dei comandi. Il prossimo passo è la previsione con il corpo nativo, come in Penumbra.

## Prove

- **Test di protocollo:** PASS, 21. Nuovi:
  - codec del lotto di comandi;
  - coda con buffer, recupero e taglio;
  - comandi spediti insieme e simulati uno per tick su UDP.
- **Prova del nemico** (`RunGameSmoke -Enemy`, `03_archives.map`): PASS. La prova ora verifica anche che il nemico sia mostrato fra due pose ("Enemy rendered between authoritative poses").
- **Checkpoint con jitter:** in 1 esecuzione su 15 il client ha scartato i checkpoint ("Invalid native save schema"). Le altre 14 sono passate, sia con la coda sia nella modalità 0.5.1, e l'errore non era riproducibile. Da qui la nuova diagnostica: alla prossima occorrenza il registro dirà il motivo.

### Prove esistenti che erano rimaste indietro

Il primo difetto era già presente nella 0.5.1: con la stessa prova in modalità 0.5.1 falliva allo stesso modo.

- **`RunHostPlayerTest.ps1`:**
  - Il client sintetico (`NetworkTests move-client`) non confermava mai di aver applicato il mondo, quindi l'host non simulava il suo input. Ora lo conferma e svuota le code come un client vero.
  - La prova si aspettava la rimozione del giocatore 2 alla disconnessione. Da quando esiste il recupero della connessione, l'host lo conserva per il rientro del compagno; ora la prova verifica la pausa per il recupero.
- **`RunCampaignStartTest.ps1`:** il fallimento ("Original campaign intro never released controls") era causato da questo stesso lavoro, non dalla 0.5.1. La prova riusa il relay di `RunImpairedNetworkTest.ps1`; aggiungendo il jitter la firma di `Run` era cambiata e il suo relay non partiva più. Per questo falliva anche con l'eseguibile della 0.5.1, e la prima conclusione di questo resoconto ("difetto già presente") era sbagliata. Il relay ora conserva anche la firma precedente.

## Limiti

- Nessuna prova su due PC reali con questa build.
- Il jitter della prova è uniforme, da 0 a 30 ms; reti reali possono avere picchi più lunghi.

## Previsione con il corpo del motore

### Modifiche

- **HPL2, `iCharacterBody`:**
  - stato del corpo salvabile e ripristinabile (`GetPredictionState`/`SetPredictionState`) e modalità di previsione (`SetPredicting`);
  - durante la previsione `Update` muove solo lo stato del corpo: niente telecamera, entità, connessioni, aggancio alle piattaforme e posizionamento del corpo fisico;
  - i callback di collisione non spingono oggetti e non chiamano il gioco (`OnGravityCollide`, `OnHitGround`).
- **Client:** i comandi non ancora confermati vengono rieseguiti con il proprio corpo del personaggio (`NativeStepper`), partendo dalla posa autorevole e con le velocità e i limiti dell'host presi dallo snapshot. Prima il modello cinematico ricostruiva solo accelerazione e decelerazione. La previsione verticale (salti, gradini) resta quella di prima.
- **Confronto:** `AMNESIA_NET_PREDICTION=kinematic` ripristina il modello cinematico nella stessa build.

### Misure A/B

Coda dei comandi attiva in entrambi i casi; due esecuzioni per caso, come sopra.

| Rete | Previsione | Oltre 2 cm | Oltre 10 cm | Correzione media |
| --- | --- | --- | --- | --- |
| 50 ms, 5 % di perdita | cinematica | 7,9–8,3 % | 0,2–1,1 % | 0,43–0,62 cm |
| | **nativa** | **2,5–2,6 %** | **0,2–0,3 %** | **0,21–0,23 cm** |
| 40 ms, jitter 0–30 ms, 2 % di perdita | cinematica | 8,7–10,6 % | 0,6–1,4 % | 0,60–0,82 cm |
| | **nativa** | **2,7–3,8 %** | **0,3 %** | **0,31–0,32 cm** |

Rispetto alla 0.5.1, le pose corrette di oltre 2 cm passano da circa la metà a circa il 3 %. La correzione media passa da 2–2,6 cm a 0,2–0,3 cm.

Un massimo di 142 cm in un'esecuzione nativa cade nell'intervallo in cui la prova fa morire il giocatore e lo fa rinascere al checkpoint: è il teletrasporto della rinascita, non un errore di previsione. Nessuna ripetizione ha richiesto 5 ms o più (`[NET/PERF] Replica prediction` assente nei registri).

## Leve e ruote dal client

Segnalazione dei giocatori: con la 0.5.1, sul client tirare una leva o girare una ruota richiedeva spesso più tentativi.

### Cause (VERIFICATE NEL CODICE E MISURATE)

1. **Frenata a ogni tick senza mouse.** `InteractRotateBase::Update` (leve e ruote) e `InteractSlide::Update` (cassetti) azzerano la velocità quando in un tick il mouse non si è mosso. In locale il mouse produce un movimento a ogni frame. In rete, con la 0.5.1, arrivava solo un tick su due (input a 30 Hz con i delta accumulati): l'oggetto veniva frenato a tick alterni. La coda dei comandi del protocollo 17 rimuove quasi tutti i tick vuoti. Quelli rimasti, che ripetono l'ultimo comando quando la rete ritarda, ora non frenano più: `cLuxPlayer::SetNetworkLookGap`.
2. **Clic d'interazione fuori tempo.** L'azione viaggia sul canale affidabile, separata dai comandi di movimento. L'host la applicava appena arrivata, con il giocatore 2 orientato secondo un comando più vecchio, rimasto nella coda: la mira dell'host non era quella del client e il clic poteva mancare l'oggetto. Ora `ActionRequest` porta il comando del client in cui è avvenuta (`inputSequence`), e l'host la applica dopo averlo simulato, al più 0,5 s dopo l'arrivo.

### Prova

Prova nuova, `RunGameSmoke.ps1 -Levers`, anche attraverso `RunImpairedNetworkTest.ps1`.

- **Allestimento:** l'host crea una leva (`lever_simple01`, girata di 180° come in `07_archives_cellar`) e una ruota (`valve_iron`) originali, e ci mette davanti il giocatore 2.
- **Il client:** mira, preme interagisci e muove il mouse di 0,02 rad per tick. Sulla leva: 3,2 s verso il basso, poi verso l'alto; sulla ruota: cerchi. Comandi e azione passano per la rete come quelli di un giocatore.
- **Misure:**
  - l'host misura lo stato della leva (−1/+1 a fine corsa, il valore che fa partire gli script della mappa) e l'angolo della ruota;
  - il client misura quanto ruotano le proprie copie, cioè quello che vede.
- **Riferimento senza rete:** con `AMNESIA_QA_LEVERS_LOCAL=1` la stessa sequenza la esegue il giocatore dell'host in locale.

| Caso | Leva: fine corsa raggiunte | Ruota (host) | Il client vede |
| --- | --- | --- | --- |
| Riferimento locale, senza rete | 2, a 0,38 s e 3,78 s | 188,6° | — |
| Client, protocollo 17, rete locale | 2, a 0,37 s e 3,77 s | 188,6° | leva 47°, ruota ≥ 180° |
| Client, protocollo 17, 40 ms + jitter 0–30 ms + 2 % di perdita (2 esecuzioni) | 2, a 0,37 s e 3,77 s | 188,6° | leva 47°, ruota ≥ 180° |
| Client, modalità 0.5.1, stessa rete | 1, a 1,1 s | 57° | ruota 56° |
| Client, modalità 0.5.1, rete locale (versione precedente della prova) | — | **0°** in 2 esecuzioni su 2 | — |

Con il protocollo 17 il client tira la leva e gira la ruota come il giocatore locale. Il valore lato client si ferma a 180° perché la misura arriva al massimo a mezzo giro.

Difetti della prova corretti durante il lavoro, non del gioco:
- un muro finto dietro la leva veniva attraversato dalla maniglia, che si incastrava anche per il giocatore locale;
- la mira puntava all'origine del corpo, che sta sul perno, invece del centro della maniglia;
- la rotazione della ruota era misurata sul supporto fisso invece che sul volante.

## Ancora aperto

**Host fermo durante il salvataggio automatico dopo un cambio di mappa:** 1 esecuzione su 42 con il passaggio a `01_old_archives` dal 27 ottobre, nella prova con rete degradata.
- Il registro dell'host si interrompe dentro il salvataggio automatico di `01_old_archives`, anche se il file di salvataggio risulta scritto per intero.
- Dopo non compare più nessuna riga, nemmeno le statistiche di rete che escono ogni 5 s; Windows non ha registrato crash.
- Le ultime righe potrebbero essere andate perse, rimaste nel buffer quando il processo è stato chiuso a fine prova.
- Non riprodotto in 3 ripetizioni immediate. Il codice coinvolto, salvataggio e cattura del checkpoint in parallelo, non è stato toccato in questo lavoro.
