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

## Da 2 a 4 giocatori

Il modello resta autoritativo: per ogni ospite l'host simula un `cPlayer` remoto. Il modello di QuasiInteractive (movimento deciso dal client, vedi `QUASI_INTEGRATION.md`) **non** viene adottato.

Il numero di giocatori, host compreso, è scelto sull'host con `PENUMBRA_COOP_PLAYERS` (2..4, predefinito 2).

| Livello | Implementato (protocollo 7) | Ancora da fare |
| --- | --- | --- |
| Protocollo | `Welcome` assegna l'ID (host 1, ospiti 2..4). Gli snapshot hanno da 2 a 4 giocatori, ognuno con il suo ID: `players[0]` è l'host, `players[1]` il destinatario (a cui si riferisce `acknowledgedInput`), poi gli altri ospiti in partita. Il client rifiuta snapshot e transizioni di vita indirizzati a un altro ID. | — |
| Trasporto host | `HostTransport` possiede l'`ENetHost` e smista gli eventi a una `Session` per ospite. Il peer resta in attesa fino a `Hello`, poi va alla `Session` che aspetta la sua identità (ripresa), altrimenti a una libera, altrimenti a quella di un ospite uscito volontariamente. Se non c'è posto viene rifiutato. `Session` funziona sia con il proprio socket (client, test) sia collegata al trasporto (`AttachHost`, `HandleEvent`). | — |
| Host (`CoopNetwork`) | Lo stato per ospite è in `Guest`: sessione, `cPlayer`, mani, epoch della mappa, stato pubblicato, inventario, diario, media, code di eventi, snapshot, diagnostica, vita e teletrasporti. `UpdateHost` cattura le pose di tutti a inizio tick, poi lega a turno ogni ospite connesso (`Bind`) ed esegue `UpdateGuest`, il codice per singolo ospite già collaudato. Ogni ospite ha il suo epoch: una riconnessione non fa ricaricare la mappa agli altri. Gli ospiti sono piazzati in punti liberi attorno all'host. | — |
| Client | Una cronologia per ogni altro ospite (`otherHistory`), riprodotta sulla linea temporale dell'host. Una silhouette per ID (`avatars`), rimossa quando quel giocatore lascia lo snapshot. Previsione solo per sé. | — |
| Vita | Rinascita accanto a un compagno vivo qualsiasi; checkpoint quando tutti sono a terra da 5 s. | — |
| Nemici | Bersaglio scelto ancora fra host e primo ospite; `EntityState::target` porta già l'ID. | Passo 5: bersagli, danni, `PlayerForBody` e attacchi ad area per tutti. |
| Salvataggi | Checkpoint del solo primo ospite (`SavePartner`/`LoadPartner`). | Passo 6: stato per ID nel checkpoint. |
| Agganci del motore | Fuori dal ciclo resta legato il primo ospite: gli agganci non ancora estesi (nemici, salvataggi) vedono lui. `IsRemoteContext`, `RemoteSimpleSwing`, `RemoteExamineDown`, `PlayerForBody` e `DamagePlayer` valgono per ogni ospite. | Passo 5. |
| Slot vuoti | Il `cPlayer` di uno slot mai connesso è inattivo ma il suo corpo resta nel mondo fisico. | Passo 5: toglierlo dal mondo fisico. |

Passi:

1. ✅ ID dei giocatori nel protocollo.
2. ✅ Trasporto multi-ospite (`HostTransport`), con test di protocollo per 3 ospiti.
3. ✅ Stato per ospite sull'host.
4. ✅ Silhouette e cronologie per gli altri ospiti sul client.
5. Nemici e vita per ogni giocatore.
6. Checkpoint per ogni giocatore.
7. ✅ Prove native con 3 e 4 istanze: `RunNativeTest.ps1 -Mode party -Players 3|4`.
