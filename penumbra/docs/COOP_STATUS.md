# COOP_STATUS — stato del progetto co-op della trilogia Penumbra

Ultimo aggiornamento: 18 novembre 2026. Il documento tiene traccia di cosa funziona, dei bug noti e del lavoro rimasto rispetto all'obiettivo finale: co-op da 2 a 4 giocatori per Overture, Black Plague e Requiem.

**Legenda**

- ✅ fatto e verificato da prove automatiche (`TESTS.md`) o da misure (`docs/COOP_TEST_RESULTS.md`)
- 🟡 parziale, oppure presente nel codice ma non verificato
- ❌ mancante

Non è stata fatta una prova reale fra due PC né una partita completa della campagna.

## Penumbra: Overture

### Sessione e connessione

| Funzione | Stato | Note |
| --- | --- | --- |
| 2 giocatori | ✅ | |
| 3–4 giocatori | 🟡 | protocollo 7, trasporto, movimento, silhouette, nemici, attacchi ad area e checkpoint per tutti (prova `party` con 2, 3 e 4 istanze sullo stesso PC); inventario ancora comune; si attiva solo con `PENUMBRA_COOP_PLAYERS` sull'host, non dal launcher |
| IP diretto (UDP 27120), LAN, VPN | ✅ | prove locali; mai provato su due PC via Radmin |
| Ospitare/entrare dal menu del gioco | ❌ | si fa dal launcher (solo italiano) |
| Lobby, inviti e relay Steam | ❌ | solo presenza Steam (App ID 22180); richiede uno studio dei requisiti Steamworks |
| Elenco giocatori e stato connessione | ❌ | |
| Riconnessione automatica | ✅ | |
| Migrazione dell'host | ❌ | |
| Versioni diverse rifiutate | ✅ | protocollo 7 |

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
| Inventari separati | ❌ | oggi l'inventario è condiviso |
| Una copia dei consumabili per ogni giocatore | ❌ | |
| Oggetti unici tracciati, dare e lasciare oggetti | ❌ | |
| Note e diario per giocatore | 🟡 | ricostruiti sul client; le note lette non sono salvate per giocatore |

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
| Stato per giocatore (inventari) | ❌ | |

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
