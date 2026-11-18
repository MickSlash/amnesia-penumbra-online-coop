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

## Progetto: da 2 a 4 giocatori (non ancora implementato)

Il modello resta autoritativo: per ogni ospite l'host simula un `cPlayer` remoto, come fa oggi per il partner. Il modello di QuasiInteractive (movimento deciso dal client, vedi `QUASI_INTEGRATION.md`) **non** viene adottato.

| Livello | Oggi | Con 4 giocatori |
| --- | --- | --- |
| `NetSession` | Un solo `peer_`; coda di input, identità, Pronto, `MapReady`/`WorldApplied` per un solo ospite | Stato per peer (`PeerState`): coda di input, cronologia, identità, Pronto, epoch confermati. Il client continua ad avere un solo peer (l'host). `Welcome` assegna uno **slot** 1..3 (host = 0), riusabile dopo un'uscita (come gli ID di Q, NM:6261). |
| Snapshot | `players[2]` fisso, 141 byte | Numero di giocatori più gli stati con lo slot; `acknowledgedInput` per slot. Ogni ospite legge il proprio slot e mostra gli altri come partner. |
| Host (`CoopNetwork`) | `remotePlayer`/`remoteHands` | Un array di slot con `cPlayer`, `cPlayerHands`, coda di input e stato di vita. `UpdateHost` cicla sugli slot. I giocatori restano tutti nel gruppo di collisione 1. |
| Client | Un partner (`partnerHistory`, avatar) | Una cronologia e un avatar per ogni altro slot; previsione solo per il proprio. |
| Nemici | `EntityState::target` 1 = host, 2 = client | Slot del bersaglio (0-3); `GetNetworkTarget` restituisce il `cPlayer` di qualsiasi slot. |
| Vita e salvataggi | Partner salvato con `SavePartner` | Stato per slot nel checkpoint (salute, batteria, inventario personale quando ci sarà); respawn accanto a un giocatore vivo qualsiasi. |
| Ingresso e uscita | Riconnessione dello stesso partner | Ingresso in corsa con baseline per il nuovo peer; uscita che libera lo slot e lascia gli oggetti unici nel mondo (con l'inventario per giocatore). |

Scelta di implementazione per il trasporto (decisa il 18/11): invece di riscrivere `Session` in versione multi-peer, si separa il trasporto.

- Un oggetto `Transport` lato host possiede l'`ENetHost` e smista gli eventi:
  - CONNECT: assegna uno slot libero e associa il peer a una `Session` dedicata;
  - RECEIVE: passa i dati alla `Session` di quel peer;
  - DISCONNECT: libera lo slot.
- Ogni ospite ha la sua `Session` con il codice per singola connessione già testato (coda di input, epoch, checkpoint, identità).
- I messaggi verso tutti (mappa, entità, media, inventario, diario, checkpoint) diventano un ciclo sulle `Session` attive.
- Il client resta invariato: un solo peer, cioè l'host.

Ordine previsto, ogni passo verificato con 2 giocatori prima di proseguire:

1. Slot ID nel protocollo, ancora con 1 ospite.
2. `NetSession` multi-peer, con test di protocollo per 3 peer.
3. Array di slot sull'host.
4. Avatar e cronologie per slot sul client.
5. Nemici e vita per slot.
6. Checkpoint per slot.
7. Prove native con 3 e 4 istanze (`RunNativeTest.ps1` va esteso a N client).
