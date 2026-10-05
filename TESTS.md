# Verifiche della versione 0.2 — 5 ottobre 2026

Windows, MSVC v145 Win32, dati originali dell'installazione Steam locale di Overture. Le prove native avviano **due processi sullo stesso PC**, con profili separati e screenshot del motore. Sono prove automatiche con fixture; la campagna non è stata percorsa manualmente dall'inizio alla fine e una sessione fra due PC tramite Radmin resta da verificare.

## Protocollo

CTest: **17 gruppi PASS**, 1 suite completata in 29.27 secondi. Include 80.000 casi malformati e 20.000 ulteriori casi dei codec, limiti delle code, ruoli autorevoli, sequenze/epoch, caricamento bloccante di 6,5 secondi, riconnessione, MTU ridotto, prediction limitata e gesti affidabili del martello con valori firmati e deduplicazione. Il timeout di caricamento di 45 secondi viene provato avanzando l'orologio ENet. I 100.000 casi sono generati dal test, non pacchetti corrotti di una partita reale.

Il protocollo 2 rifiuta le build incompatibili. Il timer degli ACK viene riletto dopo il servizio ENet: un ACK appena ricevuto non deve apparire come vecchio per sottrazione unsigned. La conferma della baseline avviene dopo l'applicazione degli eventi di presentazione.

## Prove nel gioco

| Prova | Risultato |
| --- | --- |
| Cabina e introduzione originale | Comandi sbloccati, messaggi e musica presenti su entrambi |
| Movimento e bottiglia | Movimento nativo del client sull'host; presa e spostamento fisico replicati |
| Chiave e porta | Lucchetto disattivato, chiave consumata e corpi articolati della porta aggiornati su entrambi |
| Passaggio alla miniera | Cabina → esterno → portello → `level01_02_mine_entrance.dae`; barriere completate su entrambi |
| Martello originale della miniera | Raccolto attraverso l'interazione del client, presente nei due inventari |
| Equipaggiamento separato | Entrambi nello stato melee nativo con martello; prima persona locale e modello del partner |
| Colpi manuali e semplificati | Animazione locale del client e danno deciso dall'host attraverso il combattimento originale |
| Casse rotte da entrambi | Due casse di asset originale distrutte; oggetti rimossi e dieci frammenti ciascuno sulla replica |
| Oggetto trattenuto durante la rottura | Stato Push nativo del partner rilasciato prima della distruzione del corpo |
| Torce | Due torce attive, modello del partner visibile, fascio allineato alla mira nativa e batteria del client aggiornata |
| Steam facoltativo | Init/Shutdown e tracciamento dei due processi come App ID 22180 |
| Launcher | Validazione della cartella originale e del runtime Steam a 32 bit superata |

La fixture attiva il cancello del passaggio esterno e usa l'interazione originale del portello. Nella miniera raccoglie il martello vero, crea due casse `modern_mine_woodcrate.ent` per provare i colpi e una terza per la protezione della presa durante la distruzione. La replica delle rotture comprende anche l'invio dell'effetto di polvere; gli altri effetti della campagna restano parziali. I test ignorano l'input hardware dell'utente.

Tutte le quattro prove finali terminano con PASS su entrambi, uscita nativa pulita e memoria residua zero secondo il memory manager HPL. Questo rapporto non equivale a una verifica con AddressSanitizer.

## Ritardi e movimento

Invio previsto fino a 60 Hz per i giocatori e 30 Hz per gli oggetti, entro la frequenza del gioco. I timer conservano il resto e tollerano l'arrotondamento del timestep, evitando un intervallo involontario di 33 ms. La correzione XZ conserva gravità, velocità verticale e forze. I gesti del martello usano messaggi affidabili; l'animazione in prima persona inizia sul client prima della conferma dei danni.

Gli intervalli seguenti usano **steady_clock in tempo reale** fra gli stati applicati dal thread del client, in un tratto di movimento di otto secondi logici. Non sono tempi fra datagrammi catturati sulla scheda di rete né una misura completa del ritardo mouse/schermo.

| Misura | Locale, colpi semplificati | Relay, colpi manuali |
| --- | --- | --- |
| Intervallo medio degli stati | 16.67 ms | 18.69 ms |
| Intervallo massimo | 18 ms | 51 ms |
| Stati campionati | 480 | 428 |
| Salti forzati della posizione | 0 | 0 |
| Massimo spostamento XZ per tick | 0.05 m | 0.085 m |

Il relay aggiunge **35 ms per direzione**, quindi 70 ms di andata/ritorno, e scarta ogni ventesimo datagramma: 8177 ricevuti, 7769 inoltrati e 408 scartati. Nessuno supera il tetto di 1.200 byte; massimo 1199 byte. RTT del client a fine prova: 125, 127, 126 ms, comprensivo di elaborazione e scheduling delle due istanze.

Dal colpo autorevole che esaurisce la salute alla rimozione della cassa sul client: 95 ms per la cassa colpita dal client e 236 ms per quella colpita dall'host. Questa comparazione usa l'orologio comune dei due processi sullo stesso PC; non va applicata senza sincronizzazione degli orologi a log di PC diversi. Il tempo naturale per caricare e sferrare il colpo è separato.

Non sono stati rilevati salti forzati nel tratto campionato. Il risultato non garantisce assenza di micro-scatti su ogni PC, VPN o scena; le interazioni autorevoli richiedono comunque la rete. I log `[COOP/PERF]` misurano il lavoro dell'adattatore di replica, non l'intero rendering del gioco.

## Identificazione delle prove

- Binario: SHA256 `48411A038F93C192D5A8A06958E0112FB3783DCF30C4282C64B880F0AF8B2964`.
- Martello locale: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-123220`.
- Martello con relay: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-123325`.
- Interazioni e Steam: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-123441`.
- Introduzione senza Steam: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-123531`.

`validation.json` associa le prove e le misure agli hash del binario e del launcher; `manifest.json` identifica il commit dei sorgenti corrispondenti. Il pacchetto provato non ha DLL commerciali accanto al suo eseguibile: usa quelle di `redist`.

## Limiti

Restano da completare o validare scelta dei bersagli nemici, danni/morte/respawn del player 2, combattimento contro tutti i nemici, salvataggio e ripresa co-op, notebook completo, altri strumenti, tutte le interazioni e presentazioni della campagna. Avatar procedurale e animazione approssimata del martello del partner. La neve dell'esterno resta solo sull'host. Alcune strutture di journal/checkpoint/lobby nei test del trasporto non sono integrate in Overture.

I log conservano diagnostiche originali: fallback dello shader motion blur, joint/submesh della lampada della cabina e alcuni asset ambientali mancanti della miniera. Non costituiscono una certificazione della campagna.
