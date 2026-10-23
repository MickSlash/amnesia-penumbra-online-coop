# Amnesia: The Bunker — Online Co-op: rapporto iniziale (Fase 1)

Data: 2026-10-23. Gioco analizzato: versione Steam 1.04 in
`F:\SteamLibrary\steamapps\common\Amnesia The Bunker`. Nessun file del gioco
è stato modificato; l'add-on vive nella sua cartella `mods/OnlineCoop`.

## 1. Stato reale del gioco e fattibilità

| Domanda | Risposta verificata |
|---|---|
| Motore | **HPL3** di Frictional (non Unity). Exe nativo chiuso `AmnesiaTheBunker.exe` (x64), fisica Newton, audio FMOD. |
| Logica di gioco | Quasi tutta in **AngelScript** (`script/**/*.hps`, sorgenti in chiaro): creature, armi, inventario, stati del giocatore, menu. |
| Supporto mod ufficiale | Sì: cartella `mods/`, tipi `StandAlone` e `AddOn` (`entry.hpc`), Workshop Steam, `LevelEditor`, `WorkshopModUploader`. Un add-on aggiunge directory di risorse con **priorità sui file del gioco**: uno script con lo stesso percorso sostituisce l'originale (esempio ufficiale `MinimalCustomAddOn`). |
| Rete | **Nessuna API di rete** negli script (`hps_api.hps`: nessun socket/HTTP). Il motore non ha multiplayer. |
| Canale verso l'esterno | File: `iXmlDocument::CreateFromFile` / `SaveToFile` con percorso assoluto, `cSystem_FileExists`, `cSystem_GetSystemSpecialPath`, `cSystem_GetApplicationTime`. Gli appunti (clipboard) esistono ma sono invasivi: scartati. |

**Conclusione: il progetto è fattibile, con un'architettura diversa da quella
di Amnesia TDD.** Là avevamo i sorgenti del motore e la rete è dentro l'exe; qui
l'exe è chiuso, quindi la rete sta in un **processo esterno (bridge)** e il
gioco ci parla tramite file. Il prezzo è la latenza del canale locale (misurata
in Fase 2) e il fatto che si può sincronizzare solo ciò che gli script vedono —
che in Bunker è quasi tutto il gameplay.

Scartato: iniettare codice nell'exe o agganciare funzioni native (DLL proxy,
patch in memoria). È fragile a ogni aggiornamento, può urtare anti-tamper e
contraddice il vincolo di non aggirare protezioni non necessarie.

## 2. Strumenti

- Gioco Steam ufficiale (exe Steam, con Steam aperto) e il suo menu Mods.
- Add-on AngelScript (`bunker/mod/OnlineCoop`), installato da `tools/InstallMod.ps1` con backup della copia precedente.
- Bridge C# (`bunker/bridge/CoopBridge.cs`), compilato con il `csc` di .NET Framework presente in Windows (`tools/BuildBridge.ps1`), niente SDK.
- Rete del bridge (Fase 3): UDP con canale affidabile, riusando il disegno già collaudato nel co-op di Amnesia (host autorevole, sequenze, ack, snapshot).
- Log: `hpl.log` del gioco (righe `[COOP]`), log del bridge (`[BRIDGE]`).

## 3. Mappa dei sistemi (dove vive ogni cosa)

| Sistema | File / classe | Note per il co-op |
|---|---|---|
| Registro moduli | `config/Modules.cfg`, `script/modules/ModuleInterfaces.hps` | L'add-on aggancia il modulo 17 `ModEventHandler` (pensato per le mod) invece di aggiungere un modulo: niente modifica a `Modules.cfg`, che cambierebbe a ogni aggiornamento. |
| Inventario | `custom/modules/ItemHandler.hps` (`cScrItemHandler`: `AddItem`, `RemoveItem`, `RemoveTypeFromInventory`, `GetItemCount`, `GetItem`, `SetAmount`, istanze `eInventoryInstance_Player` / `_Auxiliary`), tipi in `config/Inventory.cfg` | Unico punto di passaggio di raccolte e consumi: è qui che si intercettano le modifiche per l'inventario condiviso. |
| Munizioni di riserva | tipo oggetto `Ammunition` (pistola) e `ShotgunAmmunition` (fucile), `Bulk`, nascosti nell'inventario | Sono **oggetti d'inventario**: vanno esclusi esplicitamente dalla condivisione. |
| Tamburo del revolver | `custom/props/Prop_Gun.hps` (`cScrPropGun`, `vChamberStates`), helper `Revolver_GetUnspentBulletCount`, `Revolver_LoadChamber` in `custom/helpers/helper_gun.hps` | Stato dell'arma di ciascun giocatore: personale per costruzione. |
| Mira / ricarica | `custom/player/PlayerState_UsingGun_Aim.hps`, `PlayerState_UsingGun_Reload.hps`, `custom/modules/BulletHandler.hps` | Colpi e impatti da replicare come eventi. |
| Creatura | `custom/agents/agent_stalker.hps` (+ `agent_shotgunner`, `agent_rat`), `custom/modules/AgentBlackboard.hps` | IA in script: sull'host gira normale, sul client va spenta e pilotata dagli snapshot. |
| Rumori | `cLuxMap::BroadcastSoundHeardEvent(...)` | Il punto in cui un suono "viene sentito" dall'IA: i rumori del client si rigiocano sull'host con questa chiamata. |
| Salvataggi | `custom/modules/SaveRoomHandler.hps`, salvataggi in `Documents/My Games/Amnesia The Bunker/<profilo>/` | Solo l'host salva; il client riceve lo stato. |
| Generatore, morte, terrore | `GeneratorModule.hps`, `DeathHandler.hps`, `TerrorHandler.hps` | Stato globale dell'host. |
| Menu mod | `custom/modules/MenuHandler.hps` → `cLux_GetModHandler().LaunchMod_New(id, addOns)` | Il gioco si rilancia con `-addonbatch "<file>"` (formato `<AddOnBatch><AddOn Locator=…/></AddOnBatch>`). |

## 4. Architettura

```
 PC host                                         PC client
 ┌───────────────┐   file XML   ┌──────────┐ UDP ┌──────────┐  file XML  ┌───────────────┐
 │ Bunker + add-on│ <──────────> │ bridge   │<───>│ bridge   │<─────────> │ Bunker + add-on│
 │ (autorevole)   │ game_out/in  │ (host)   │     │ (client) │            │ (replica)      │
 └───────────────┘              └──────────┘     └──────────┘            └───────────────┘
```

- **Canale locale:** `game_out.xml` (scritto dal gioco ogni 50 ms) e `game_in.xml` (scritto dal bridge con file temporaneo + rename, quindi atomico). Ogni messaggio ha un numero di sequenza: un file riletto o letto a metà viene scartato. Senza bridge l'add-on resta inattivo e il gioco è identico all'originale.
- **Autorità:** l'host decide creatura, porte, generatore, inventario condiviso, salvataggi. Il client invia input ed eventi ("ho raccolto X", "ho sparato", "rumore in P") e riceve stato.
- **Inventario condiviso contro munizioni personali**
  - *Condiviso*: tutti i tipi di `Inventory.cfg` **tranne** una lista di esclusione esplicita: `Ammunition`, `ShotgunAmmunition` e le armi stesse (`Gun`, `Shotgun`), perché ciascuno ha il proprio revolver. L'host tiene il registro unico. Il client che raccoglie invia una *richiesta*, applicata solo se l'host la conferma. Così due raccolte simultanee dello stesso oggetto non lo duplicano mai.
  - *Personale*: riserva (`Ammunition` nell'inventario di ciascuno) e tamburo (`vChamberStates` del proprio `cScrPropGun`). Non viaggiano mai nel registro condiviso. Una scatola di proiettili raccolta va a chi la raccoglie e viene distrutta per entrambi: un solo evento dell'host, nessuna copia.
  - Verifica: l'add-on calcola ogni secondo un riepilogo (oggetti condivisi + riserva + colpi nel tamburo, per giocatore) e lo registra in log. I test confrontano i totali prima e dopo per dimostrare l'assenza di duplicazioni.
- **Creatura:** sul client l'IA dello stalker viene disattivata; posizione, animazione e stato arrivano dall'host a 20 Hz con interpolazione. Danni e uccisioni sono eventi dell'host.

## 5. Piano a fasi

| Fase | Contenuto | Verifica |
|---|---|---|
| 1 | Ricognizione (questo documento) | — |
| 2 | Add-on caricabile + heartbeat + bridge che fa eco, misura del canale | self-test del bridge; `[COOP/IPC]` nel log del gioco con RTT |
| 3 | Rete tra due bridge (UDP, ack, riconnessione), handshake e versione, launcher | **fatto**: `CoopBridge --test-net`, 11 verifiche su 11 superate con rete pulita, con 5% di perdita / 60 ms / 30 ms di jitter, e con 20% / 150 ms / 80 ms. Controllo negativo: senza riordino degli eventi il test fallisce. Nel gioco reale il link arriva al modulo (`[COOP/NET] link Idle -> Connected`) |
| 4 | Giocatore remoto visibile (avatar, posizione, animazione base) | **fatto**: soldato francese (`soldier_alive`) con le animazioni del giocatore (fermo, camminata in 4 direzioni, corsa, accovacciato) scelte in base alla direzione di marcia, braccia corrette via script. Verificato con due istanze sullo stesso PC e con un compagno simulato (`--fake-walker`) |
| 5 | Inventario condiviso + munizioni personali | test 1-5 della specifica (duplicazioni, raccolte simultanee) |
| 6 | Creatura con autorità dell'host, rumori del client sull'host | test creatura, rumore, morte |
| 7 | Mondo: porte, generatore, oggetti fisici, salvataggi solo dell'host | test salvataggio/caricamento, cambio mappa |
| 8 | UI (stato connessione, compagno), pacchetto, robustezza | i 12 test della specifica |

## 6. Primo incremento concreto (Fase 2: fatto)

- `bunker/mod/OnlineCoop`: `entry.hpc` (AddOn), override di `script/modules/ModEventHandler.hps` (classe originale intatta, più 5 righe `[COOP]`) e `script/coop/OnlineCoop_Core.hps`: heartbeat ogni 50 ms con mappa e posizione, lettura dell'eco, statistiche ogni 10 s.
- `bunker/bridge/CoopBridge.cs`: eco con scrittura atomica, statistiche, `--selftest`.
- Verifiche eseguite:
  - self-test del bridge: 190 messaggi su 190 con eco, RTT medio 17 ms;
  - **nel gioco reale** (exe Steam, Steam aperto, add-on attivato come fa il menu Mods). Il modulo compila e trova il bridge. In mappa (`trenches`) l'heartbeat parte con posizione del giocatore; il bridge riceve circa 198 messaggi ogni 10 s, nessuna sequenza persa, nessuna lettura fallita. **RTT gioco → bridge → gioco: medio 16 ms**, minimo 2, massimo 19 ms, con un singolo picco a 97 ms. Costo I/O nel gioco: 50-140 ms ogni 10 s (0,5-1,4% del tempo). Il picco iniziale di 9,7 s è il caricamento della mappa, non il canale.
- Fatti scoperti durante le prove (servono alle fasi successive):
  - il documento `iXmlDocument` *è* l'elemento radice: gli attributi vanno scritti e letti direttamente sul documento;
  - il `tString` del motore espone solo `length()`/`resize()`, quindi ogni operazione sulle stringhe passa da `cString_*`;
  - il menu Mods rilancia il gioco con `-addonbatch "<file>" -user <profilo>`, con `<AddOnBatch><AddOn Locator="local:OnlineCoop" /></AddOnBatch>`; con gli add-on attivi i log e i salvataggi vanno in `MainWithAddons/<hash>/`;
  - un exe avviato da fuori Steam viene riavviato da Steam senza argomenti, mentre `steam -applaunch` toglie le virgolette e il batch viene ignorato. `tools/LaunchWithMod.ps1` replica il rilancio del gioco stesso: exe con `SteamAppId` nell'ambiente, Steam aperto e licenza verificata normalmente.
- Lezioni della Fase 4, tutte verificate in gioco:
  - **attivazione della mod**: il gioco accetta `addons.batch` solo in `Documents/My Games/Amnesia The Bunker/Main/`, il file scritto dal suo menu; un batch altrove viene ignorato senza nessun avviso;
  - **stato del modulo nei salvataggi**: le proprietà dello script vengono salvate e ripristinate, con gli handle XML azzerati; dopo un caricamento il modulo si ricostruisce da solo, e `OnMapEnter` non viene chiamato per la mappa caricata;
  - **avatar nei salvataggi**: le entità create da script finiscono nello stato della mappa anche con `FullGameSave=false`; l'avatar ha un nome unico per sessione, viene seguito per ID e le copie ripristinate vengono eliminate;
  - **animazioni condivise**: una risorsa animazione è condivisa da tutte le entità che la caricano, quindi dichiararla con un altro strato o usarla su uno scheletro diverso rovina anche il corpo del giocatore (mira e movimenti). L'avatar usa copie private, create all'installazione dai file del gioco di chi installa e non distribuite con la mod;
  - **strati delle braccia** (`LeftArm`/`RightArm`): il motore li maschera solo per il corpo del giocatore, quindi l'avatar usa solo lo strato `Body` e le braccia hanno una correzione via script (`SetPostTransform`), calibrata dal vivo con `avatar_tune.xml`;
  - **orientamento della mesh**: `soldier_alive` guarda verso +Z, quindi serve mezzo giro rispetto alla telecamera; Lambert, usato prima, non ne aveva bisogno.
- Da migliorare: oggi il gioco rilegge `game_in.xml` a ogni frame (60 letture/s anche senza novità). In Fase 3 la lettura avverrà solo quando cambia la data di modifica del file.
