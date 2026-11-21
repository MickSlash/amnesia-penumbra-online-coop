# COOP_STATUS — stato del progetto co-op della trilogia Penumbra

Ultimo aggiornamento: 20 novembre 2026. Il documento tiene traccia di cosa funziona, dei bug noti e del lavoro rimasto rispetto all'obiettivo finale: co-op da 2 a 4 giocatori per Overture, Black Plague e Requiem.

**Legenda**

- ✅ fatto e verificato da prove automatiche (`TESTS.md`) o da misure (`docs/COOP_TEST_RESULTS.md`)
- 🟡 parziale, oppure presente nel codice ma non verificato
- ❌ mancante

Le prove automatiche `full`, `hammer`, `campaign` e `resume` passano anche fra due PC reali collegati con Radmin VPN (21 novembre, `COOP_TEST_RESULTS.md` §18). Non è stata giocata a mano una partita fra due PC né una partita completa della campagna.

## Penumbra: Overture

### Sessione e connessione

| Funzione | Stato | Note |
| --- | --- | --- |
| 2 giocatori | ✅ | |
| 3–4 giocatori | 🟡 | trasporto, movimento, silhouette, nemici, attacchi ad area e checkpoint per tutti (prova `party` con 2, 3 e 4 istanze sullo stesso PC); inventari e diario per giocatore; l'host sceglie 2-4 giocatori nel launcher |
| IP diretto (UDP 27120), LAN, VPN | ✅ | prove automatiche su due PC via Radmin VPN (client in Wi-Fi): RTT mediano 19-27 ms, nessuna perdita rilevante |
| Schede Intel HD (driver OpenGL Intel) | ✅ | gli shader con letture "ombra" involontarie vengono ricompilati al caricamento: bloom e qualità "Alta" corretti sulla HD 4400 |
| Ospitare/entrare dal menu del gioco | ❌ | si fa dal launcher (solo italiano) |
| Lobby, inviti e relay Steam | ❌ | solo presenza Steam (App ID 22180); richiede uno studio dei requisiti Steamworks |
| Elenco giocatori e stato connessione | ❌ | |
| Riconnessione automatica | ✅ | |
| Migrazione dell'host | ❌ | |
| Versioni diverse rifiutate | ✅ | protocollo 8 |

### Movimento e rete

| Funzione | Stato | Note |
| --- | --- | --- |
| Host autorevole, tick fisso 60 Hz | ✅ | |
| Coda di input con ridondanza, un comando per tick, ACK del comando simulato | ✅ | protocollo 4, 18/11/2026 |
| Previsione con le regole native del movimento (diagonale, intensità, corsa, accovacciato, salto) | ✅ | test unitari; misure in `COOP_TEST_RESULTS.md` |
| Diagnostica `[COOP/NETDIAG]` | ✅ | RTT, jitter, correzioni, deriva, percorsi |
| Previsione durante il trascinamento di oggetti, porte e leve | ❌ | il client segue l'host con un RTT di ritardo |
| Buffer di interpolazione temporale per partner, oggetti e nemici | ✅ | protocollo 5 |
| Collisioni coerenti fra host e client | ✅ | replay con il corpo nativo; gruppo di collisione dei giocatori; collider dei nemici sul client |

### Personaggi

| Funzione | Stato | Note |
| --- | --- | --- |
| Partner visibile | 🟡 | sagoma provvisoria fatta di box |
| Modello umano e animazioni | ❌ | |
| Torcia del partner (modello, luce, orientamento) | ✅ | |
| Martello del partner | 🟡 | animazione approssimata |
| Glowstick del partner | 🟡 | non verificato |
| Passi e audio posizionale del partner | 🟡 | non verificati |

### Interazione e fisica

| Funzione | Stato | Note |
| --- | --- | --- |
| Prendere e trascinare oggetti | ✅ | eseguito dall'host |
| Porte, chiave, lucchetto, casse rotte | ✅ | eseguito dall'host |
| Risposta immediata sul client | ❌ | ogni interazione passa dalla rete |
| Proprietà degli oggetti, passaggio di autorità | ❌ | |

### Inventario e risorse

| Funzione | Stato | Note |
| --- | --- | --- |
| Salute, batteria ed equipaggiamento per giocatore | ✅ | |
| Inventari separati | ✅ | uno per giocatore; torcia, glowstick, armi, taccuino e mappa a tutti, il resto a chi lo raccoglie (`COOP_ARCHITECTURE.md`) |
| Consumabili per giocatore | ✅ | batterie, antidolorifici, razzi, oggetti da lanciare e cibo restano a chi li raccoglie |
| Oggetti unici tracciati | ✅ | chiavi e oggetti della storia restano a chi li trova; gli script li cercano in tutto il gruppo |
| Dare oggetti a un compagno | ✅ | oggetto scelto, mira al compagno, clic; una unità delle pile; prova `full` in entrambe le direzioni |
| Lasciare oggetti a terra | ❌ | |
| Diario personale | ✅ | note in comune; la nota si apre solo a chi la raccoglie, gli altri vedono un avviso; ognuno ha le sue note da leggere (in rosso), salvate per giocatore; il taccuino aperto non viene chiuso da aggiornamenti e messaggi |

### Morte e transizioni

| Funzione | Stato | Note |
| --- | --- | --- |
| Morte individuale, respawn vicino al compagno dopo 5 s | ✅ | |
| Morte di tutti: ricarica del checkpoint co-op | ✅ | |
| Spettatore | ❌ | |
| Cambio mappa con barriera di caricamento | ✅ | prova `maps`: tutte le 25 mappe della campagna caricate e sincronizzate su host e client |

### Nemici

| Funzione | Stato | Note |
| --- | --- | --- |
| IA solo sull'host, valuta entrambi i giocatori | ✅ | cane, ragno, verme |
| Danni al giocatore giusto | ✅ | |
| Animazioni, salute e morte replicate | ✅ | |
| Musica d'inseguimento solo per chi è inseguito | 🟡 | non verificata |
| Nascondersi nelle situazioni reali | 🟡 | non provato |

### Script e storia

| Funzione | Stato | Note |
| --- | --- | --- |
| Script eseguiti solo sull'host | ✅ | |
| Effetti, messaggi, suoni e nebbia inviati al client, anche per un singolo giocatore | ✅ | |
| Tutti gli enigmi e i finali | 🟡 | non verificati |

### Salvataggi

| Funzione | Stato | Note |
| --- | --- | --- |
| Checkpoint co-op separato, scrittura atomica, controllo d'integrità | ✅ | |
| Ripresa con processi nuovi, rientro dello stesso partner | ✅ | |
| Copia del salvataggio sul client | ❌ | |
| Stato per giocatore (inventari) | ✅ | prove `campaign` e `resume` |

## Black Plague e Requiem

| Elemento | Stato |
| --- | --- |
| Installazione Steam | `E:\Steam\steamapps\common\Penumbra Black Plague\redist` (`Penumbra.exe`, `Requiem.exe`, Requiem in `expansion01`) |
| Codice di gioco | non pubblico |
| Eseguibili Steam | protetti dal DRM di Steam (`.bind`, `.text` cifrata): non vanno analizzati |
| Versione per l'analisi | l'utente possiede la versione GOG (senza DRM); da installare e verificare prima dell'analisi con REA o Ghidra |
| Script dei livelli | 43 file `.hps` leggibili in chiaro |
| Lavoro di porting | non iniziato |

## Altri documenti

- `COOP_ROADMAP.md`: fasi e priorità, compito corrente e prossimi passi.
- `COOP_KNOWN_ISSUES.md`: difetti aperti.
- `COOP_TEST_RESULTS.md`: analisi, comandi di prova e misure.
- `COOP_ARCHITECTURE.md`: come funziona la rete e dove sta il codice.
