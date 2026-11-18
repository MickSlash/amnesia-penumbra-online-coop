# COOP_ARCHITECTURE — rete co-op di Penumbra: Overture

## Ruoli

- **Host:** esegue il gioco vero: fisica Newton, IA, script, salvataggi. Il personaggio del client esiste sull'host come secondo `cPlayer` (`remotePlayer`) mosso dai comandi ricevuti.
- **Client:** esegue lo stesso gioco come "replica":
  - nemici senza IA;
  - oggetti fisici statici, posizionati dall'host;
  - script che non modificano il mondo condiviso.

  Mostra subito il proprio movimento grazie alla previsione.

## Trasporto

ENet su UDP (porta 27120), 4 canali (`game/network/NetSession.*`). Ogni messaggio inizia con la versione del protocollo, e le versioni diverse vengono rifiutate.

| Canale | Tipo | Contenuto |
| --- | --- | --- |
| 0 | affidabile | controllo, mappa, azioni, snapshot di riserva |
| 1 | non affidabile | input (`InputBatch`), snapshot dei giocatori, pose degli oggetti |
| 2 | affidabile | entità, inventario, diario, presentazione, media, eventi di vita |
| 3 | affidabile | checkpoint |

## Movimento del giocatore client (protocolli 4–5)

1. Il client invia a ogni tick (60 Hz) gli ultimi 6 comandi (`InputBatch`).
2. L'host li accoda (`network/InputQueue.h`):
   - ne simula esattamente uno per tick;
   - un buffer adattivo da 1 a 4 comandi assorbe il jitter;
   - gli arretrati si recuperano con un comando in più per tick;
   - oltre mezzo secondo di arretrato i comandi vengono scartati, unendo i delta.
3. L'host cattura la posa del partner **prima** di applicare il comando del tick e conferma l'ultimo comando già integrato dalla fisica (`acknowledgedInput`).
4. Il client parte dalla posa dell'host e rigioca i comandi non confermati con il proprio `iCharacterBody` in modalità previsione (`NativeStepper` in `CoopNetwork.cpp`, `iCharacterBody::SetPredicting`). Le piccole correzioni si smorzano in circa 80 ms (`network/Prediction.h`).

## Personaggi e collisioni

- I due giocatori sono nel gruppo di collisione 1 (`SetCharacterGroup`): si attraversano fra loro e urtano i nemici come nell'originale.
- Sul client i nemici replicati hanno il corpo inattivo; al loro posto ci sono collider statici (`enemyColliders`).

## Partner, oggetti e nemici sul client (protocollo 5)

- Snapshot e pose portano il tick dell'host (`hostTick`).
- Il client li riproduce sulla linea temporale dell'host (`network/Interpolation.h`): ritardo adattivo da 3 a 15 tick, interpolazione fra due pose note, nessuna estrapolazione.

## Diagnostica

- `[COOP/NETDIAG]` ogni 5 s in `hpl.log` (`network/NetDiagnostics.h`); campi descritti in `COOP_TEST_RESULTS.md` §2.
- `[NET/STATS]` per il trasporto.

## Prove

- `tests/network/NetworkTests.cpp` (CTest `ProtocolValidation`): protocollo, codifiche, coda, previsione, interpolazione, UDP reale.
- `tests/network/RunNativeTest.ps1`: due istanze del gioco su questo PC.
  - Modalità: `baseline`, `full`, `intro`, `hammer`, `campaign`, `resume`.
  - Opzioni: `-OneWayDelayMs`, `-JitterMs`, `-DropEvery`, `-Hitch`, `-Walk`.

## Riuso per Black Plague e Requiem

Trasporto, coda di input, previsione, interpolazione e diagnostica non dipendono dal gioco. Gli agganci (`Capture`, `ApplyEntity`, `UpdateHost/UpdateClient`) sono in `CoopNetwork.cpp` e andranno adattati per ogni titolo.
