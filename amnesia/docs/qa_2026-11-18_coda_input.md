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

Entrambi i difetti erano già presenti nella 0.5.1. Li ho verificati con la stessa prova in modalità 0.5.1: falliva allo stesso modo.

- **`RunHostPlayerTest.ps1`:**
  - Il client sintetico (`NetworkTests move-client`) non confermava mai di aver applicato il mondo, quindi l'host non simulava il suo input. Ora lo conferma e svuota le code come un client vero.
  - La prova si aspettava la rimozione del giocatore 2 alla disconnessione. Da quando esiste il recupero della connessione, l'host lo conserva per il rientro del compagno; ora la prova verifica la pausa per il recupero.
- **`RunCampaignStartTest.ps1`:** fallisce ("Original campaign intro never released controls") anche con l'eseguibile della 0.5.1. Resta aperto, da indagare a parte.

## Limiti

- Nessuna prova su due PC reali con questa build.
- Il jitter della prova è uniforme, da 0 a 30 ms; reti reali possono avere picchi più lunghi.
