# Log della co-op di Penumbra: Overture

I percorsi vengono dal codice:
- `tools/CoopLauncher.cs`: `state` e `PENUMBRA_COOP_PROFILE_ROOT`;
- `game/CoopProfile.cpp`: `CoopPersonalDirectory`;
- `game/Init.h`: `PERSONAL_RELATIVEGAME`;
- `game/Init.cpp`: `SetLogFile`.

## Dove sono

**Partita avviata dal launcher** (il caso normale). Il launcher imposta
`PENUMBRA_COOP_PROFILE_ROOT=%LocalAppData%\PenumbraOnlineCoop\<ruolo>`, e il
gioco aggiunge `PenumbraOnlineCoop\Overture\`.

| Ruolo | Log | Dove |
| --- | --- | --- |
| Host | Log principale | `%LocalAppData%\PenumbraOnlineCoop\host\PenumbraOnlineCoop\Overture\hpl.log` |
| Host | Log di aggiornamento del motore, di solito vuoto | `...\host\PenumbraOnlineCoop\Overture\hpl_update.log` |
| Client | Log principale | `%LocalAppData%\PenumbraOnlineCoop\client\PenumbraOnlineCoop\Overture\hpl.log` |
| Entrambi | Sessioni precedenti | `hpl.1.log` (l'ultima) e `hpl.2.log` (quella prima), nella stessa cartella |
| Entrambi | Impostazioni | `settings.cfg` nella stessa cartella |
| Host | Checkpoint co-op | `...\host\PenumbraOnlineCoop\Overture\save\coop_checkpoint.sav` |
| Launcher | Preferenze | `%LocalAppData%\PenumbraOnlineCoop\*.txt`: cartella del gioco, Steam, giocatori, schermo intero, impostazioni importate |

Il pulsante **Apri log della mod** del launcher apre
`%LocalAppData%\PenumbraOnlineCoop`. Ogni PC ha i log del proprio ruolo: per
analizzare una partita servono quello dell'host, dal PC dell'host, e quello
del client, dal PC del client.

**Senza launcher**, cioè senza `PENUMBRA_COOP_PROFILE_ROOT`: il log è in
`Documenti\PenumbraOnlineCoop\Overture\hpl.log`.

**Prove automatiche**:
- su un PC: `build\qa\coop-<data-ora>\host\...\hpl.log` e
  `client\...\hpl.log`, più `client2` e `client3` con 3-4 giocatori;
- su due PC: `build\qa\twopc-<data-ora>\`, con il log del client copiato dal
  secondo PC in `client\`.

Il gioco originale scrive invece in `Documenti\Penumbra Overture\Episode1\`;
la mod non tocca quei file.

## Cosa contiene

Un solo file, `hpl.log`, raccoglie i log del motore, del gioco e della mod.
- **Orario:** ogni riga inizia con l'orario locale `[hh:mm:ss.mmm]`.
- **Righe di rete:** quelle `[NET/...]` hanno anche `[t=<ms>]`, l'orologio
  monotono del processo.
- **Prima riga:** è `[COOP/SESSION]`, con l'identificativo della sessione, il
  ruolo (`host`, `client`, `single`), il protocollo e la data.

| Categoria | Origine | Contenuto |
| --- | --- | --- |
| Senza prefisso, per esempio "Loading map", "CG:", "WARNING", "ERROR" | Motore HPL1 e gioco originale | Caricamenti, shader, risorse mancanti, audio (OpenAL) |
| `[COOP/SESSION]` | Mod | Inizio sessione |
| `[NET/CONNECTION]`, `[NET/MAP]`, `[NET/STATS]`, `[NET/DIAG]`, `[NET/STATE]` | Rete | Connessione, ritardo (RTT) e perdite, pacchetti, pause del thread principale |
| `[COOP/NETDIAG]` | Rete, ogni 5 s | Previsione, correzioni, intervalli fra stati, ritardo di riproduzione |
| `[COOP/PERF]` | Mod, ogni 5 s | Costo della co-op per aggiornamento (`average_us`, `max_us`) |
| `[COOP/FPS]` | Gioco, ogni 5 s, anche in gioco singolo | FPS, tempo medio e massimo del fotogramma, fotogrammi oltre 33 ms, suoni presenti e attivi |
| `[COOP/MAP]`, `[COOP/STATE]`, `[COOP/AVATAR]` | Mod | Mappe, applicazione del mondo, compagni |
| `[COOP/LIFE]`, `[COOP/SPECTATOR]` | Mod | Morte, rinascita, spettatore |
| `[COOP/INVENTORY]`, `[COOP/HINT]`, `[COOP/JOURNAL]`, `[COOP/SAVE]` | Mod | Oggetti passati, avvisi, diario, salvataggi |
| `[COOP/STEAM]` | Mod | Presenza Steam |
| `[COOP/LOBBY]` | Mod | Lobby: apertura, Pronto, stato dei giocatori, avvio, attesa dei caricamenti, inizio della partita |
| `[COOP/RENDER]` | Tasto F9 | Stato degli effetti grafici (bloom, scia, profondità di campo, nebbia, menu, messaggio) e nome dello screenshot salvato nella cartella del log |
| `[COOP/QA]`, `[COOP/SOUND]` | Solo nelle prove automatiche | Esiti delle prove; elenco dei suoni attivi |

## Riconoscere i problemi

- **Desincronizzazione:**
  - `[COOP/NETDIAG]` sul client, con `poses_over_10cm`, `resets_over_25cm`,
    `hard_over_2m` maggiori di zero o `correction_cm_max` alto;
  - "Waiting to apply world" in `[COOP/MAP]`;
  - "Rejected", "invalid ..." in `[NET/CONNECTION]`.
- **Rete lenta o persa:**
  - `rtt_ms` alto in `[NET/STATS]`;
  - `reliable_loss_per_mille` maggiore di zero;
  - "Peer disconnected or timed out".
- **Blocchi del gioco:**
  - "Transport service paused on game thread for N ms" in `[NET/DIAG]`;
  - `frame_ms_max` e `frames_over_33ms` in `[COOP/FPS]`;
  - `max_us` in `[COOP/PERF]`.

  I picchi di circa 1 s durante i caricamenti delle mappe sono normali.
- **Eccezioni e crash:**
  - righe "FATAL ERROR" o "ERROR:" nel log;
  - un log che finisce senza "HPL Exit was successful!";
  - i dump in `%LocalAppData%\CrashDumps\PenumbraCoop.exe.*.dmp`, se
    Windows li crea.
- **Oggetti che scattano** (client), in `[COOP/NETDIAG]`:
  - `entity_updates_per_s`: aggiornamenti di oggetti ricevuti al secondo;
  - `entity_gap_ms`: intervallo fra due pose dello stesso oggetto;
  - `object_jerk_cm`: variazione del passo fra tick consecutivi, cioè lo
    scatto visibile; le righe `Object jerk` danno oggetto e situazione dei
    picchi sopra 3 cm;
  - `send_kbps`, `receive_kbps`: banda.
- **Grafica degradata:** premere F9 quando succede, poi ESC, tornare in
  gioco e premere di nuovo F9: le due righe `[COOP/RENDER]` e i due
  screenshot `render_*.bmp` mostrano cosa cambia.
- **Audio:**
  - la sezione "Initializing Sound Module" indica dispositivo e voci (32);
  - `sounds_playing` in `[COOP/FPS]` vicino a 32 vuol dire voci esaurite;
  - durante le prove `[COOP/SOUND]` elenca i suoni attivi, per trovare i
    doppioni.

## Chi ha fatto cosa

- **Il ruolo** è in `[COOP/SESSION]`. Le righe della mod lo ripetono come
  `role=host` o `role=client`.
- **Il giocatore:** gli ID sono 1 per l'host e 2-4 per gli ospiti
  (`Player assigned ID`, `player=2`, `target=2`).
- **Allineare i due PC:** si usa l'orario di ogni riga. Gli orologi dei PC
  possono differire di qualche secondo: conviene ancorarsi a un evento che
  compare in entrambi, come "Peer connected" o "Loaded epoch=N".

## Log più dettagliati

Variabili d'ambiente, da impostare prima di avviare `bin\PenumbraCoop.exe`:
- `PENUMBRA_COOP_QA=<modo>` attiva le prove automatiche e i loro log
  (`[COOP/QA]`, `[COOP/SOUND]`);
- `PENUMBRA_COOP_FINAL_SCREENSHOT=<file.bmp>` salva il fotogramma finito,
  quello che vede il giocatore, dopo `PENUMBRA_COOP_DEBUG_FRAME` fotogrammi
  (predefiniti 900);
- le opzioni originali di debug del gioco restano in `settings.cfg`, sezione
  `Debug`.
