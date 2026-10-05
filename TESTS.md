# Verifiche della versione 0.3 — 5 ottobre 2026

Windows, MSVC v145 Win32, dati originali dell'installazione Steam locale di Overture. Le prove native avviano **due processi sullo stesso PC**, con profili isolati, fixture e screenshot del motore. Non sono una partita manuale completa né una prova fra due PC tramite Radmin.

## Protocollo

CTest: **17 gruppi PASS**, una suite completata in 31.36 secondi. Include 80.000 casi malformati e 20.000 ulteriori casi dei codec, limiti delle code, ruoli autorevoli, sequenze/epoch, caricamento bloccante di 6,5 secondi, riconnessione, MTU ridotto, prediction limitata e gesti affidabili del martello. Sono casi generati dal test, non pacchetti corrotti di una partita reale.

Il protocollo **3** aggiunge salute, bersaglio, stato, posa del corpo e loop delle animazioni dei nemici, più la richiesta di checkpoint del client. Rifiuta build precedenti, salute negativa, bersagli inesistenti e stati nemici fuori dai limiti. La salute dei nemici conserva anche il valore elevato del verme originale. Le pose ordinarie passano senza affidabilità; baseline, eventi di vita e cambiamenti essenziali usano messaggi affidabili.

## Prove native nel gioco

| Prova | Risultato |
| --- | --- |
| Introduzione normale | Comandi sbloccati, messaggi e musica presenti su entrambi |
| Movimento, bottiglia, chiave e porta | Movimento nativo, presa fisica, chiave consumata, lucchetto e porta articolata replicati |
| Passaggio alla miniera | Cabina → esterno → portello → `level01_02_mine_entrance.dae`; barriere completate |
| Martello originale | Raccolto tramite interazione del client, presente nei due inventari; equipaggiamento separato |
| Colpi manuali e semplificati | Combattimento nativo sull'host; casse rotte da entrambi e dieci frammenti per cassa sulla replica |
| Oggetto trattenuto durante la rottura | Presa rilasciata prima della distruzione del corpo |
| Torce | Due torce attive, modello del partner, fascio allineato alla mira e batteria aggiornata |
| Cane originale | Attacchi e danni reali dell'IA nativa a player 1 e player 2 |
| Ragno e verme originali | IA nativa con bersaglio e danno al player 2 |
| Replica nemici | Animazioni osservate sul client; IA indipendente e reazioni ai trigger disattivate sul client |
| Morte individuale | Host e client muoiono e tornano vicino al compagno vivo dopo cinque secondi |
| Nemico morto | Danno letale nativo, corpo ragdoll e ripristino del cadavere dal checkpoint |
| Checkpoint nella sessione | Salute, batteria, martello, torcia e posizione di entrambi ripristinati; casse rotte, nemici, notebook e variabile script conservati |
| Ripresa con processi nuovi | Profilo dell'host copiato; nuovi host/client ritrovano entrambi i giocatori e il mondo salvato |
| Morte di entrambi | Ripresa automatica del checkpoint e nuova baseline applicata sui due processi |
| File non valido o alterato | XML incompleto e checksum modificato rifiutati prima di azzerare il mondo |
| Scrittura fallita | File temporaneo bloccato: checkpoint precedente conservato byte per byte |
| Steam e launcher | Due processi tracciati come App ID 22180; validazione cartella originale e runtime Steam superata |

Le **sette prove finali** terminano con PASS su entrambi, codice di uscita zero, chiusura nativa pulita e memoria residua zero secondo il memory manager HPL. Non è una verifica con AddressSanitizer.

La fixture del martello attiva il cancello dell'esterno e usa l'interazione originale del portello. Nella miniera raccoglie il martello vero e crea casse da asset originali per provare i colpi. La fixture dei nemici crea cane, ragno e verme originali in un'area controllata della stessa mappa, aggiungendo nodi di navigazione per quella prova. Le singole morti dei giocatori e il danno letale al cane vengono anche indotti dal test: non equivalgono a una vittoria manuale in combattimento contro tutti i nemici.

Il checkpoint usa il salvataggio nativo completo, esteso al secondo giocatore. La fixture verifica un cane vivo disattivato a 25 HP, un cane morto, una cassa distrutta con dieci frammenti, un task del notebook e una variabile globale. Salute iniziale salvata: 67/43; batteria: 73/61. Dopo il ripristino la rigenerazione e il consumo della torcia continuano normalmente. La prova di ripresa usa processi nuovi e una copia del profilo isolato dell'host, senza accedere ai salvataggi personali.

## Ritardi e movimento

Invio previsto fino a 60 Hz per i giocatori e 30 Hz per il mondo, entro la frequenza effettiva del gioco. La visuale e l'inizio del colpo del client sono locali; danni, porte e altri esiti autorevoli richiedono la rete.

Gli intervalli usano **steady_clock in tempo reale** fra gli stati applicati dal thread del client, durante otto secondi logici di movimento. Non misurano l'intero ritardo mouse/schermo né i datagrammi sulla scheda di rete.

| Misura | Locale, colpi semplificati | Relay, colpi manuali |
| --- | --- | --- |
| Intervallo medio degli stati | 20.3 ms | 18.91 ms |
| Intervallo massimo | 34.0 ms | 50.0 ms |
| Stati campionati | 394 | 423 |
| Salti forzati della posizione | 0 | 0 |
| Massimo spostamento XZ per tick | 0.092 m | 0.125 m |

Il relay aggiunge **35 ms per direzione**, quindi 70 ms di andata/ritorno, e scarta ogni ventesimo datagramma. Nella prova del martello: 8160 ricevuti, 7752 inoltrati e 408 scartati. Nessun datagramma oltre 1.200 byte; massimo 1199 byte. RTT finale del client: 124, 125, 126 ms, comprensivo di elaborazione e scheduling delle due istanze.

Dal colpo autorevole letale alla rimozione della cassa sul client: 82 ms per la cassa colpita dal client e 73 ms per quella colpita dall'host. Si confronta l'orologio comune dei due processi; su PC diversi occorre sincronizzare gli orologi. Il tempo per caricare e sferrare il colpo è separato.

Anche nemici, respawn e checkpoint superano una prova col relay: 21992 datagrammi ricevuti e 1099 scartati, massimo 1200 byte. Queste misure non garantiscono assenza di scatti su ogni PC, VPN o scena. I log di replica non misurano l'intero rendering.

## Identificazione delle prove

- Binario: SHA256 `C10041C9C26C316D7F0EF0851C3A722071E860DA84EE6CBDF11F4C196124A177`.
- Launcher: SHA256 `F3C566F91E9B3141505F099FC62701A86A59173252916A796E1703F1BBD45CEE`.
- `campaign03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-143843`.
- `campaign-impaired03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-144638`.
- `resume03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-144836`.
- `full03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-144849`.
- `intro03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-144938`.
- `hammer03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-145003`.
- `hammer-impaired03`: `C:\Codice\AmnesiaCoop\PenumbraOnlineCoop\build\qa\coop-20261005-145109`.

`validation.json` associa risultati, percorsi e misure agli hash dei due eseguibili; `manifest.json` identifica il commit archiviato in `PenumbraOnlineCoop-source.zip`. Il pacchetto non contiene DLL commerciali o dati del gioco: usa l'installazione originale `redist`.

## Limiti

Restano da verificare la campagna intera, tutte le arene e le situazioni di nascondiglio, ogni arma e consumabile, gli enigmi e gli eventi audio/visivi non coperti, e partite su due PC reali. I test non certificano tutti i combattimenti o la fluidità su Radmin.

Il client non conserva una copia autonoma del checkpoint; nessuna migrazione host. La cronologia del notebook viene ricostruita, ma le note già lette non hanno uno stato personale persistito separatamente. Avatar procedurale e animazione approssimata del martello del partner; neve dell'esterno soltanto sull'host. I vecchi salvataggi senza stato del secondo giocatore e integrità non sono compatibili.

I log conservano alcune diagnostiche originali degli asset e dello shader motion blur. Il risultato è una versione sperimentale verificata nelle fixture descritte, non una certificazione dell'intera campagna.
