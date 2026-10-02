# Amnesia Online Co-op — architettura e stato

## Stato verificabile

Prima tappa: ENet UDP, handshake, controllo versione, input serializzato e test
host/client in processi distinti, anche con due eseguibili del gioco. L'host crea
un `cLuxPlayer(1)` senza viewport split-screen e riceve intenti di movimento.
Il client inoltra input tastiera/mouse a 30 Hz e trattiene interazioni e movimento
locale; manca ancora il ritorno di snapshot per la sua camera/corpo.
Non equivale a un MVP giocabile: snapshot,
interpolazione, mondo, inventario, AI client e transizioni non sono ancora replicati.
Non usare questa build per una campagna online e non salvare sessioni sperimentali.

## Punti di inserimento verificati nel codice locale

* `LuxBase.cpp`, `InitGame`: P1 è un modulo `cLuxPlayer` nel container Default.
  `mpPlayer2` nasce nullo; `LuxMapHandler.cpp::CreatePlayer2` costruisce un secondo
  `cLuxPlayer(1)`, chiama Reset/CreateWorldEntities/OnMapEnter e lo posiziona vicino a P1.
* `LuxInputHandler.cpp::UpdateGameInput` richiama entrambi i percorsi input.
  `UpdatePlayer2Input` gestisce gamepad e `UpdatePlayer2RawInput` gestisce seconda
  tastiera/mouse Windows. Il punto sicuro per l'input remoto è prima di entrambi:
  il percorso locale viene escluso in online. L'applicazione effettiva degli
  intenti avviene nel tick della mappa prima di UpdatePlayer2, indipendentemente
  dallo stato input/UI di P1. Si riusano Move/AddYaw/AddPitch/Jump/Crouch/Run.
* `LuxMapHandler.cpp::UpdatePlayer2` esegue Update e PostUpdate di P2; P1 è
  aggiornato dal sistema dei moduli. Non registrare P2 anche come modulo (doppio tick).
* `LuxPlayer.cpp::CreateWorldEntities` crea il character body Newton e gli helper,
  compreso l'avatar. Camera, avatar e `LuxPlayerHands` sono presentazione separata.
  `SetCoopMode` crea viewport split-screen: non va usato come architettura online finale.
* Health e sanity sono campi di ciascun `cLuxPlayer`; la lanterna è un helper per
  giocatore. `LuxInventory` resta un modulo condiviso con `GetActivePlayer` e
  strutture per risorse/oggetti individuali. Normal e Hard hanno politiche diverse.
* `LuxMap` indicizza entità per `iLuxEntity::GetID()` (`m_mapEntitiesByID`).
  Gli ID sono locali alla mappa: la replica futura deve associare ID host a entità
  client e includere una generazione della mappa, senza trasmettere puntatori.
* Interazioni: `LuxPlayerState_Normal`, stati InteractGrab/Push/Slide/SwingDoor/
  Lever/Wheel e `LuxProp_*`. Pickup: `LuxProp_Item`; porte fisiche: SwingDoor;
  porte di transizione: LevelDoor. Usare richiesta e risultato validati dall'host.
* Fisica: `HPL2/core/sources/impl/PhysicsWorldNewton.cpp` e CharacterBodyNewton.
  Replicare trasformazioni/velocità di corpi rilevanti, eventi e sleeping; niente lockstep.
* `LuxEnemy.cpp` ha già `GetTargetPlayer`, `UpdateTargetPlayer` e rilevamento per
  entrambi i giocatori. `GetTargetPlayer` richiede coop attivo; conservarne la semantica
  e disabilitare sul client la simulazione autorevole prima della tappa AI.
* `LuxMap.cpp::RunScript` e `LuxScriptHandler.cpp` usano AngelScript. La mod
  contiene già `CoopGetPlayerByIndex` e scambio temporaneo di `gpBase->mpPlayer`
  per agire come P2. Non conservare quel puntatore oltre un callback e non
  sostituire meccanicamente le letture del singleton. Timer memorizzano il giocatore agente.
* `LuxSaveHandler.cpp` salva flag coop e stato P2. In online il salvataggio futuro
  apparterrà all'host; non unire salvataggi e non salvare stato di socket.
* `LuxMapHandler::CheckMapChange` gestisce fade/caricamento; serve una barriera
  Prepare/Ready/Execute per mappa e spawn prima di poter giocare livelli diversi.

## Trasporto e protocollo implementato

ENet 1.3.18, sorgenti ufficiali in `third_party/enet`, licenza MIT conservata
in LICENSE. UDP con affidabilità e ordinamento forniti dalla libreria; supporto
Windows x64, dipendenze di sistema ws2_32 e winmm. Il codice della mod rimane GPLv3.

`network/NetSession` non dipende da Lux: gestisce socket, connessione, handshake,
timeout e validazione. `LuxNetwork` è l'adattatore globale del motore, aggiornato
nei tick logici anche nei menu. Il motore usa `cLogicTimer`; rendering e logica
sono distinti (`Engine.cpp`), quindi non mettere Poll nei callback grafici.

Header: due byte ASCII AO, versione uint16 big-endian, tipo uint8.
Canale 0 affidabile: Hello e Welcome (ID giocatore uint32 assegnato 2).
Canale 1 non affidabile ordinato: PlayerInput, 22 byte totali; sequence e tick uint32,
quattro valori int16 (assi *1000 e angoli radianti *1000), bit jump/crouch/run.
Pacchetti con dimensione errata, versione errata, canale inatteso o valori fuori
range sono rifiutati; nessuna deserializzazione di strutture C++ grezze.
Handshake massimo 5 s; timeout ENet massimo 10 s; input scaduto dopo 250 ms.
L'host ignora sequenze obsolete/duplicate usando aritmetica con wraparound.

## Ordine delle prossime tappe

1. Completare verifica movimento da tastiera/mouse con due istanze;
   aggiungere identità/sessione/mappa allo spawn. Lo spawn remoto ora verifica
   la shape del personaggio contro il mondo Newton, evitando offset dentro muri.
2. Snapshot dei due giocatori, camera locale full-screen, interpolazione con
   buffer temporale; mantenere una sola simulazione canonica sull'host.
3. Interazioni e pickup host, replica corpi fisici, AI e danno, script e barriera
   caricamento. Non abilitare campagne online finché questi sistemi divergono.

Nessun asset del gioco deve entrare in Git. Le prove runtime usano esclusivamente
l'installazione legittima dell'utente.

## Verifiche eseguite il 2 ottobre 2026

Build originale tentata prima delle modifiche networking. Corrette configurazione
include ImGui/SDL2, comparatore VideoComp senza std::binary_function, percorso
OpenAL relativo al progetto e puntatore PCM Vorbis non inizializzato. Ricompilate
tutte le dipendenze con v145 per evitare LTO misto v142/v145. Build baseline
riuscita senza il post-build legacy su D:. Deploy ora opt-in via AmnesiaDeployDir.

Build finale Release x64; test protocollo (troncamento, endian, range, versione);
UDP host/client in processi separati; rifiuto versione incompatibile seguito da
connessione valida; due Lux.exe con handshake e uscita pulita; host Lux.exe su
Entrance Hall con P2 creato, movimento orizzontale da un sender ENet separato e
rimozione alla disconnessione. Test runtime con profili distinti e zero memory
leak riportati dal motore. Non ancora verificata visibilità reciproca né movimento
client riconciliato: servono snapshot e interpolazione.
