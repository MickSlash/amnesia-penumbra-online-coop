# QUASI_INTEGRATION — confronto con la co-op di QuasiInteractive e piano di integrazione

## Fonte

- **Repository:** https://github.com/QuasiInteractive/penumbra-overture-coop, clonato in sola lettura in `C:\Codice\AmnesiaCoop\external\penumbra-overture-coop`, fuori dal nostro repository.
- **Versione analizzata:** v0.12 (`e034830`, 10/11/2026), protocollo 24. Storia: v0.6 (`071b762`), v0.10 (`a9a07f6`), v0.11 (`241f21a`, il grosso: 4 giocatori, browser, voce, modelli), v0.12.
- **Metodo:** analisi del sorgente reale, non del README.
  - Abbreviazioni: NM = `Overture/multiplayer/NetworkManager.cpp`, NP = `Overture/multiplayer/NetworkPackets.h`.
  - Numeri di riga riferiti a v0.12.

## Licenze

| Materiale | Licenza | Uso possibile da parte nostra |
| --- | --- | --- |
| Codice di gioco e motore | GPLv3 (`Overture/COPYING`, `HPL1Engine/COPYING`), come il codice Frictional | Compatibile con la nostra GPL-3.0-or-later. Il codice riusato mantiene autori e avvisi di copyright; nei crediti va citato "Quasi Interactive". |
| Modelli dei personaggi (`multiplayer/models`, ~78 MB) | Non dichiarata. Mesh generate con Meshy AI (`tools/README_characters.md`, tabella "source"); animazioni convertite dal pacchetto commerciale di motion capture Motifect (`tools/README_animations.md`) | **Non copiati nel nostro repository né nei nostri pacchetti.** Possiamo supportare lo stesso formato, in modo che chi possiede o scarica legittimamente i modelli possa usarli. Per ridistribuirli serve il permesso degli autori o una licenza esplicita. |
| Strumenti Python della pipeline dei personaggi | GPLv3 (nel repository) | Riusabili se serviranno. |

## Differenza di architettura (decisiva)

| | QuasiInteractive | Noi |
| --- | --- | --- |
| Movimento del giocatore | **Deciso dal client.** Ogni PC simula il proprio giocatore e invia posizione e velocità (NM:87-181). Nessun controllo di velocità o teletrasporto, solo un limite di coordinate a 20000 (NM:6290). | **Deciso dall'host.** L'host simula il personaggio del client con un vero secondo `cPlayer`; il client invia comandi, prevede con il codice di movimento nativo e viene riconciliato (protocolli 4-6, misure in `COOP_TEST_RESULTS.md`). |
| Script e trigger | Ogni PC esegue i propri script di livello; scritture di variabili, attivazioni, lucchetti, oggetti e rotture vengono replicate come eventi (GameScripts.cpp:825-1390, 2029-2066). I timer non sono replicati (README "boat horn"), e le scritture remote non eseguono le callback. | **Solo l'host esegue gli script**; il client riceve effetti, presentazione, inventario e stato del mondo. Nessuna divergenza di script per costruzione. |
| Fisica | Host autoritativo, 30 Hz. Gli ospiti guidano i corpi con **velocità obiettivo** (BodySync.h:22-42), non con teletrasporti: gli oggetti restano fisici anche sul client. | Host autoritativo. Sul client i corpi sono statici e riprodotti sulla linea temporale dell'host (protocollo 5). Prendere un oggetto è previsto rispetto alla telecamera (protocollo 6). |
| Personaggi remoti | "Ghost": modello animato con buffer di interpolazione sul clock del mittente, 100 ms (GhostPlayer.cpp:1040-1123). | Sagoma di box. Il partner viene riprodotto sulla linea temporale dell'host con ritardo adattivo da 3 a 15 tick. |
| Giocatori | Fino a 4: stella con l'host che inoltra (NM:2507-2522). Il limite reale è il numero di modelli (NM:2355). | 2 slot fissi (`Snapshot::players[2]`). |

Conseguenza: non si può "unire" il loro trasporto o la loro gestione dei giocatori al nostro senza riscrivere uno dei due modelli. **Teniamo il nostro modello autoritativo**, perché garantisce coerenza degli script e assenza di trucchi, ed è misurato. Dal loro codice prendiamo, adattandoli, i sottosistemi indipendenti dal modello di autorità.

## Confronto per sottosistema

Legenda: **Q** = solo QuasiInteractive, **N** = solo noi, **=** = entrambi.

| Sottosistema | Chi | Migliore | Evidenza | Integrazione |
| --- | --- | --- | --- | --- |
| Trasporto ENet con versione verificata alla connessione | = | Pari | NP:120, NM:2340; il nostro `NetSession.cpp` | — |
| Autenticazione con password (sfida + digest, non crittografico) | Q | Q | NP:814-881, NM:2372-2474 | **Adattabile** al nostro handshake (`Hello/Welcome`) |
| Tabella dei ruoli, limiti di frequenza, "strike", validazione dei campi | Q / parziale nostro | Q più ampio | NM:2449-2500, 6168, 6509, 6802 | **Adattabile**: noi validiamo i codec, non i limiti di frequenza |
| Diagnostica RTT, jitter, perdita, correzioni | N | N | `[COOP/NETDIAG]`; in Q nessun uso di `roundTripTime` o `packetLoss` | — |
| Test automatici | N | N | CTest con oltre 80.000 casi, prove native con due istanze, relay con ritardo, jitter e perdita; in Q solo `tools/syntax_check.sh` | — |
| Previsione e riconciliazione del giocatore | N | N | Q non le ha, perché il movimento è deciso dal client | — |
| Riconnessione automatica, recupero dell'host | N | N | `RecoverConnection`, promozione del client; Q: nessuna (NM:2391) | — |
| 3-4 giocatori, ID stabili, riuso degli ID | Q | Q | NM:2355, 6261 | **Da adattare**: va esteso il nostro protocollo (§Piano) |
| Browser LAN (broadcast) e Internet (master server) | Q | Q | NM:628-681, 4223-4494, `tools/master_server.py` | **Adattabile** (il master server richiede un host pubblico) |
| Menu nel gioco, lobby, nomi, scelta del personaggio | Q | Q | MainMenu.cpp:51-56, 986-1903 | **Da adattare** al nostro flusso (oggi gestito dal launcher) |
| Pannello del gruppo, barre della salute, notifiche | Q | Q | Player.cpp:1759, 1846 | **Adattabile** (dati già disponibili negli snapshot) |
| Chat vocale di prossimità (Opus + cattura OpenAL) | Q | Q | VoiceChat.cpp, CMakeLists.txt:192-215 | **Adattabile**; nuova dipendenza Opus |
| Personaggi animati, scelta delle clip, dissolvenze, ritmo dei passi | Q | Q | GhostPlayer.cpp:171-212, 562-741 | **Da adattare** al nostro `UpdateAvatar`; modelli non ridistribuibili (vedi Licenze) |
| Oggetti in mano ai compagni, fasci delle torce, glowstick, flare | Q / parziale nostro | Q | GhostPlayer.cpp:487-499, 562-572 | **Adattabile** |
| Passi udibili dei compagni | nessuno | — | TriggerHandler.cpp:44 ("Nothing is played") | Da fare per entrambi |
| Nemici: IA solo sull'host, più bersagli | = | Pari; Q ha l'isteresi di bersaglio | GameEnemy.cpp:1328-1561 | **Adattabile**: l'isteresi del cambio di bersaglio è da confrontare con la nostra selezione |
| Udito dei nemici per gli ospiti (passi sintetici) | Q | Q | TriggerHandler.cpp:31-45, 115+ | **Da verificare**: da noi il personaggio del client esiste sull'host come `cPlayer`, quindi probabilmente genera già i suoni nativi |
| Danni dei nemici agli ospiti | = | N più semplice | Q: box + raggio + messaggio al PC della vittima (GameEnemy.cpp:1894-1927); noi: danni nativi sull'host | — |
| Fuoco amico in mischia, opzionale | Q | Q | HudModel_Weapon.cpp:643-664 | **Adattabile** come opzione |
| Fisica sul client: velocità obiettivo invece di pose statiche | Q | Da misurare | BodySync.cpp:487-523 | **Candidato al confronto A/B**: potrebbe risolvere "oggetti statici sul client" e la spinta |
| Giocatori solidi (non si attraversano) | Q | Scelta di design | GhostPlayer.cpp:750-800 | **Configurabile**: noi abbiamo scelto che si attraversino (gruppo 1); potremmo rendere la solidità un'opzione |
| Oggetto preso: previsione sul client | = | Da misurare | Q: guidato a velocità, `kHeldGain` 14 (BodySync.h:111-119); noi: posa relativa alla telecamera, risposta 33 ms | — |
| Porte e cassetti: previsione | nessuno | — | Q: esplicitamente esclusa (BodySync.h:111-119) | Da fare |
| Ingresso a partita in corso: istantanea atomica del mondo | = | Pari | Q: NM:4991-5691; noi: baseline più checkpoint | — |
| Inventario | Q condiviso (opzionale "uno per tipo"); noi condiviso | Nessuno dei due come vogliamo noi | NM:2584-2594, GameItem.cpp:247 | Il nostro progetto prevede inventari separati: da implementare; il modello di Q ("uno per tipo", consegna fra giocatori) è un buon punto di partenza |
| Prova "HasItem" del gruppo (basta che uno abbia l'oggetto) | Q | Q | GameScripts.cpp:815 | **Necessario** quando avremo inventari separati |
| Morte e respawn | = | Pari | Q: 3 s, 40 HP, il più vicino (PlayerHelper.cpp:1255-1379); noi: 5 s, accanto al sopravvissuto, checkpoint se muoiono tutti | — |
| Diario per giocatore | nessuno | — | Q non ha codice dedicato | Da fare |
| Schermo intero senza bordi | Q | Q | LowLevelGraphicsSDL.cpp:209-236 | **Adattabile**, piccolo |
| Segnalazione dei crash (SEH) | Q | Q | Main.cpp:36-227 | **Adattabile** |

## Affermazioni non verificate o smentite (Q)

- **Ragno:** il README dice che resta a bersaglio singolo, ma il codice lo rende a più bersagli (`GameEnemy_Spider.cpp:756`). Solo il verme è a bersaglio singolo.
- **Internet:** il README dice che serve una VPN; il codice ha un browser con master server, ma niente NAT traversal, quindi serve comunque un port forwarding o una VPN.
- **Nessun test** che dimostri le affermazioni su fisica e rete; nessuna misura di latenza nel codice.
- **Strumentazione temporanea lasciata attiva** nel motore (`GHOST_LOADER_TRACE` = 1, MeshLoaderCollada.h:22-25).

## Limiti noti di Q che il nostro modello evita già

- Eventi a tempo che si sentono solo per chi li attiva: da noi gli script girano solo sull'host, e la presentazione (suoni, messaggi, effetti) viene inoltrata.
- Scritture remote di variabili che non eseguono le callback: non applicabile, perché da noi c'è un solo esecutore di script.

## Piano di integrazione (ramo `quasi-integration`)

In ordine di valore e di rischio. Ogni passo: compilazione, test di protocollo, prove native, misure prima e dopo, commit locale.

1. **Fisica degli oggetti sul client:** confronto A/B fra le nostre pose riprodotte e i loro corpi guidati a velocità (BodySync). Lo scopo è risolvere la spinta degli oggetti e la risposta delle porte. Si adotta solo se le misure migliorano.
2. **Personaggio animato del partner:** il loro sistema di clip e dissolvenze adattato a `UpdateAvatar`.
   - Ripiego sulla sagoma se i modelli mancano.
   - Formato compatibile con la loro cartella `multiplayer/models`; nessun file ridistribuito.
   - Oggetti in mano e luci sul partner.
3. **Da 2 a 4 giocatori nel nostro protocollo:**
   - slot e ID fino a 4;
   - un `cPlayer` remoto per ogni ospite sull'host;
   - snapshot e input per giocatore;
   - salvataggi, respawn e transizioni per N giocatori.

   Riferimento per la gestione degli ID: NM:2355, 6261.
4. **Interfaccia nel gioco:** voce "Multiplayer" nel menu principale, ospitare e entrare, nomi, lobby, pannello del gruppo, barre della salute, notifiche; poi il browser LAN.
5. **Sicurezza:** password con sfida, limiti di frequenza, tabella dei ruoli.
6. **Chat vocale di prossimità**, con la dipendenza Opus e un'opzione di build come in Q.
7. **Gameplay previsto da noi e assente in entrambi:** inventari separati, oggetti unici tracciati con consegna, consumabili per giocatore, diario per giocatore, spettatore. Prova "HasItem" del gruppo ispirata a GameScripts.cpp:815.
8. **Opzioni configurabili:** giocatori solidi, fuoco amico, bottino condiviso.

## Registro dell'integrazione

### 1. Corpi del client guidati a velocità (BodySync) — provato, non adottato come predefinito

- **Implementazione:** `DriveBody`/`PinBody` in `game/CoopNetwork.cpp`, adattati da `BodySync.cpp:480-931` (termine proporzionale verso la posa riprodotta, velocità stimata dal bersaglio, limiti diversi per i corpi con giunti, scatto oltre 1,5 m o se incastrato, blocco a riposo). Attivabile con `PENUMBRA_COOP_BODY_DRIVE=1`; il bersaglio è la nostra posa riprodotta sulla linea temporale dell'host.
- **Misure A/B** (stesso eseguibile, una prova per caso):

| Misura | Guida a velocità | Pose riprodotte (predefinito) |
| --- | --- | --- |
| Girarsi tenendo la bottiglia, locale / RTT 120 ms | 33 / 33 ms | 33 / 33 ms |
| Anta dell'armadietto, locale / RTT 120 ms | 166 / 383 ms | 150 / 366 ms |
| Camminata hammer locale: pose > 2 cm (finestra di ritorno) | 187 | 190 |
| Camminata hammer RTT 120 ms + jitter 40 ms + 5 % perdita: pose > 10 cm | 16 | 14 |
| Costo `UpdateClient` (hammer locale) | 1,3–1,6 ms | 1,1–1,5 ms |

- **Decisione:** nessun miglioramento misurabile nei casi provati, quindi resta disattivata. Da rivalutare quando ci sarà una prova di spinta degli oggetti camminando, l'unico caso dove la fisica locale del client dovrebbe aiutare.

### 2. Correzioni del motore (Math, AnimationTrack) — integrate

- **Da `HPL1Engine/sources/math/Math.cpp`:**
  - la slerp dei quaternioni interpola davvero anche le rotazioni quasi identiche (prima restituiva la chiave precedente: le mesh animate avanzavano a scatti sulle chiavi);
  - gestisce il caso antipodale;
  - nessuna divisione per intervalli nulli.
- **Da `HPL1Engine/sources/graphics/AnimationTrack.cpp`:** niente accessi fuori dal vettore con tracce vuote, tempi NaN o tempi oltre l'ultima chiave.
- **Verifica:** test di protocollo e tutte le prove native (hammer locale, con jitter e con frame lenti, full, intro, campagna, campagna con ritardo) PASS. Attribuzione in `THIRD_PARTY.md`.
