# Amnesia Online Co-op — architettura e stato

## QA successiva

La [verifica estesa del 2 ottobre](qa_2026-10-02.md) aggiunge screenshot di livelli
originali, prova del Grunt, stress ASAN e rete con ritardo/perdita. Ha trovato
consegna intermittente dello stato di morte e differenze di cutscene/fog/HUD.
Il passaggio delle prove precedenti non certifica la robustezza di questi casi.

## Stato implementato

La branch `feature/online-coop` contiene una prima integrazione giocabile per due
processi: l'host simula entrambi i giocatori, Newton, nemici, interazioni e script;
il client invia intenti e visualizza il mondo autorevole a schermo intero.
La campagna completa non è ancora certificata: le prove automatiche coprono
Entrance Hall e Old Archives, non una partita dall'inizio ai titoli di coda.

Implementati:

- Snapshot a 20 Hz di posizione, orientamento, altezza occhi, salute, sanità,
  olio, acciarini, lanterna, crouch, morte e stato attivo dei due giocatori.
  Buffer client da 100 ms, interpolazione e correzione dei teletrasporti.
  Orientamento della camera locale immediato; movimento senza prediction.
- Replica di props, aree e nemici tramite ID host e generazione della mappa,
  trasformazioni dei corpi, animazioni, attivazione, distruzione, luci dei props,
  porte bloccate/chiuse e creazione dinamica dagli asset installati.
- Intenti affidabili di interazione, acciarino, attacco, lanterna, uso e combinazione degli
  oggetti. L'host riusa raycast, distanza e stati originali: il client non sceglie
  un bersaglio né invia trasformazioni da applicare al mondo.
- Inventario condiviso autorevole e uso di oggetti nel contesto di P2.
- Cambio livello e ricaricamento del salvataggio con barriera Map/Ready,
  baseline completa del mondo e isolamento degli eventi fra mappe.
- Salvataggi appartenenti all'host con stato di P2; morte e checkpoint condivisi.
- Replica affidabile dei nuovi eventi di voce, messaggio, nota e diario;
  delta di colore, raggio, visibilità e attivazione delle luci esistenti.
- Avvio con profili separati tramite `tools/StartOnlineCoop.ps1`.

## Autorità e integrazione del motore

`network/NetSession` non dipende da Lux: gestisce ENet, handshake, code e codec.
`LuxNetwork` è un modulo globale aggiornato anche nei menu. L'input remoto è
applicato prima di `LuxMapHandler::UpdatePlayer2`, senza registrare P2 come secondo
modulo e senza abilitare split-screen. Lo spawn prova la shape Newton vicino a P1.

Sul client P1 interno rappresenta il giocatore di rete 2 e P2 interno l'avatar
dell'host, ID 1. La simulazione Newton, gli aggiornamenti autorevoli di mappa e
player e l'esecuzione degli script mappa/globale/inventario sono esclusi. Camera,
mani, lanterna, avatar e presentazione continuano ad aggiornarsi.
Il mondo online resta attivo durante inventario e journal tramite il modulo
globale, conservando un solo aggiornamento della simulazione.

L'host resta proprietario degli script e degli effetti sul gameplay. Il client
non esegue una seconda copia di AngelScript. Gli eventi di presentazione coprono
solo i tipi elencati: questo non implica che ogni funzione script della campagna
abbia già una replica. Una disconnessione client elimina il mondo replica e torna
al menu; una disconnessione host libera P2. Non è implementata la ripresa della
sessione o la migrazione dell'host.

## Protocollo versione 2

ENet 1.3.18 incluso da sorgenti, licenza MIT conservata; mod GPLv3. Header AO,
versione uint16 big-endian e tipo uint8; interi espliciti, nessuna struttura C++
trasmessa in memoria. Tre canali:

| Canale | Affidabilità | Contenuto |
| --- | --- | --- |
| 0 | Affidabile e ordinato | Hello/Welcome, Map/MapReady, Action |
| 1 | Ordinato, non affidabile | PlayerInput a 30 Hz (26 byte), Snapshot a 20 Hz (71 byte) |
| 2 | Affidabile e ordinato | Entity, WorldReady, Inventory, Presentation |

Ogni stato gameplay include un'epoch di mappa. Sequenze obsolete o duplicate
sono ignorate con aritmetica che supporta il wraparound. MapReady abilita snapshot
e intenti; WorldReady e applicazione della baseline abilitano l'input client.
Entità iniziali assenti dal roster host vengono rimosse dal client.

Handshake massimo 5 s, timeout ENet fino a 10 s, intenti di movimento scaduti
in 250 ms, caricamento client massimo 30 s. Poll non bloccante limitato a 64 eventi;
code limitate e pacchetti ENet massimi di 64 KiB. Codec controllano dimensioni,
range, tipi, stringhe, percorsi e canale/direzione. Gli asset restano locali.
La fingerprint FNV-1a di `.map` e `.hps` verifica compatibilità di quei due file;
non è una verifica crittografica né copre tutti gli asset, configurazioni e script
inclusi. Entrambi i PC devono usare la stessa versione della build e dei dati.

Il roster viene inviato con massimo 64 cambiamenti per tick. I corpi del mondo
usano delta affidabili, senza interpolazione fisica client; sotto perdita elevata
il backlog può aumentare. Le code piene chiudono la sessione invece di consentire
una crescita illimitata. Questo comportamento va ancora misurato su WAN.

## Verifica e limiti

Release x64 compilata con MSVC v145. I test di protocollo verificano endian,
troncamenti, byte aggiunti, range, percorsi, sequenze, timeout input, isolamento
fra mappe, inventario, baseline ed eventi della storia. I test UDP in processi
separati verificano handshake, rifiuto della versione errata e disconnessione.

`RunGameSmoke.ps1 -Campaign` avvia due Lux.exe con profili di prova separati e
asset dell'installazione legittima. Controlla movimento P2 e camera client,
baseline, raccolta di un acciarino una sola volta, consumo di una pozione con
salute aggiornata, inventario client, luci e messaggio, salvataggio/ricaricamento
con stato remoto conservato, cambio Entrance Hall → Old Archives e morte di P2
con ritorno di entrambi al checkpoint. È una prova automatica delle integrazioni,
non una verifica visiva o un attraversamento manuale dei puzzle della campagna.
Voci e documenti hanno codec e hook implementati, ma richiedono prova manuale
con contenuti reali e apertura/chiusura del journal.

Prima di dichiarare un MVP dell'intera campagna servono:

1. Partita completa su due PC, con audit dei trigger, puzzle, inseguimenti,
   teletrasporti, cutscene e finali. Verifica separata della modalità Hard.
2. Copertura degli effetti script ancora mancanti: fog, effetti speciali,
   suoni non vocali, particelle, trasformazioni/creazione di luci e tipi di entità
   non compresi nel roster attuale (per esempio rope/commentary).
3. Stato iniziale di journal/storia per join dopo un salvataggio o una raccolta:
   gli eventi nuovi sono replicati, la cronologia precedente non è trasferita.
4. Prove con latenza/perdita WAN e misure di banda; prediction del movimento e
   interpolazione dei corpi se necessarie. Attualmente il movimento client
   include ritardo di rete e buffer da 100 ms.
5. Verifica manuale di grab, porte, leve, combinazioni e nemici a due bersagli,
   oltre ai casi automatizzati. Reconnect, lobby e NAT traversal restano fuori.

Non commettere binari né dati proprietari. Build e configurazioni generate sono
ignorate da Git; i salvataggi del launcher usano cartelle dedicate per ruolo.
