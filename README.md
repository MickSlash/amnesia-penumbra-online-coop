# Penumbra: Overture — Online Co-op 0.2

Prototipo sperimentale Windows per due giocatori. Ricostruisce i sorgenti ufficiali di Overture e HPL1 e usa i dati della propria installazione originale. Il progetto è separato dalla mod di Amnesia.

## Installazione e avvio

1. Entrambi estraggono **tutto** lo ZIP della mod in una cartella separata.
2. Aprono `PenumbraCoopLauncher.exe` e selezionano la cartella di Overture. Accetta sia la cartella principale sia `redist`.
3. L'host sceglie **Ospita partita**; l'altro inserisce l'IP dell'host e sceglie **Connettiti all'host**.
4. La porta predefinita è **UDP 27120**. I due launcher devono usare la stessa porta e la stessa versione della mod.

Non servono comandi PowerShell per giocare. Il pacchetto non sostituisce gli eseguibili originali e non contiene mappe, texture, audio o DLL commerciali del gioco. Il launcher usa le DLL della propria cartella `redist`. Se manca il runtime MSVC x86, installare il Microsoft Visual C++ Redistributable x86; se manca OpenAL, usare l'installer incluso nel gioco originale.

I comandi restano quelli di Overture, configurabili nelle opzioni: WASD per muoversi, mouse per guardare, tasto di interazione per raccogliere/afferrare, inventario per selezionare la chiave e click sul lucchetto per usarla. Per trascinare porte e cassetti tenere premuto il tasto di interazione e muovere il mouse.

Il martello raccolto nella miniera entra nell'inventario condiviso: entrambi possono equipaggiarlo separatamente. I colpi del client iniziano subito nella sua visuale; l'host decide danni e rotture e invia agli altri lo stato, i frammenti e l'effetto di polvere. La torcia del partner ha un modello visibile e proietta luce su entrambi i PC. Per colpire usare i controlli originali del martello: tenere premuto il tasto di interazione, muovere il mouse per caricare e invertire il gesto per sferrare il colpo; è disponibile anche l'opzione originale dei colpi semplificati.

**Aggiornare entrambi i PC alla 0.2**: il protocollo è versione 2 e rifiuta build incompatibili. Invia gli stati dei giocatori fino a 60 Hz e quelli degli oggetti fino a 30 Hz, entro la frequenza effettiva degli aggiornamenti del gioco. La prediction riduce l'attesa del proprio movimento; conferme delle interazioni e rotture restano soggette alla rete.

## Steam facoltativo

**Usa Steam** registra il processo come **Penumbra: Overture, App ID 22180**. Serve Steam aperto con l'account che possiede Overture. Per DVD o altre edizioni lasciare l'opzione disattivata: la connessione co-op funziona indipendentemente da Steam.

La versione Steam di Overture installata su questo PC non include `steam_api.dll`. Il launcher cerca automaticamente un runtime Valve originale a 32 bit, già presente nelle librerie Steam; il gioco ne verifica la firma prima di caricarlo. Nessun runtime Valve viene redistribuito. Se il PC contiene soltanto Overture e non dispone di questo runtime, disattivare l'opzione. La scelta viene ricordata.

Integrazione basata su [SteamAPI_Init e SteamAPI_Shutdown](https://partner.steamgames.com/doc/sdk/api), verificata anche nei log del client Steam. Non sono implementati inviti Steam o matchmaking.

## LAN, Radmin e Hamachi

Su LAN usare l'IP locale dell'host; su Radmin/Hamachi entrambi devono stare nella stessa rete VPN e il client deve inserire l'IP VPN dell'host. Consentire `bin/PenumbraCoop.exe` nel firewall per quella rete. Su LAN/VPN normalmente non serve inoltrare porte sul router; per collegamento diretto da Internet occorre rendere raggiungibile UDP 27120.

Sono state provate due istanze sullo stesso PC, anche attraverso un relay con 35 ms di ritardo per direzione e circa 5% di perdita. Una sessione reale fra due PC tramite Radmin/Hamachi resta da verificare.

## Funzioni e limiti

Implementati: host autorevole, ingresso nella stessa mappa, avatar provvisorio, movimento con prediction e correzione che conserva gravità e impulsi, oggetti fisici e corpi articolati replicati, raccolta e inventario condiviso, uso della chiave, mouse per trascinare porte, martello con equipaggiamento indipendente e colpi autorevoli, rotture e frammenti replicati, torcia del partner e batteria del client sincronizzata, messaggi/fade e replica iniziale di suoni e musica, cielo/nebbia/illuminazione ambientale, cambio mappa e tentativi di riconnessione.

Verificati nel gioco: cabina iniziale, movimento del client, presa/spostamento della bottiglia, chiave sul lucchetto, apertura dell'armadietto, mappa esterna e miniera con il martello. Il test usa due processi sullo stesso PC, posiziona i giocatori e aziona input di prova. La prova della miniera raccoglie il vero martello e crea due casse di un asset originale per verificare i colpi di entrambi. **Non equivale a una partita manuale completa della campagna.**

Ancora da completare o validare:

- Nemici che scelgano e attacchino entrambi i giocatori, danni/morte del secondo giocatore, combattimento contro ogni nemico, nascondersi e respawn.
- Salvataggio/ripresa dei due giocatori, notebook/journal completo, consumo degli oggetti, combinazioni, altre armi e tutte le interazioni della campagna.
- Animazioni dell'avatar e dei nemici, effetti/particelle, presentazione completa delle scene e tutti gli eventi audio.
- Ripresa dopo perdita prolungata, migrazione host e test della campagna dall'inizio alla fine.

Il launcher avvia una nuova partita. Il menu online offre ripresa della sessione, opzioni ed uscita; caricamento dei salvataggi online non disponibile. Gli autosalvataggi nativi dell'host non contengono ancora lo stato completo del secondo giocatore. Non usare questa versione per una campagna lunga contando sulla ripresa dei progressi.

## Log e profili

Il pulsante **Apri log della mod** apre `%LocalAppData%\PenumbraOnlineCoop`. I log del gioco sono in:

```
host\PenumbraOnlineCoop\Overture\hpl.log
client\PenumbraOnlineCoop\Overture\hpl.log
```

Profili e salvataggi della mod sono separati dal gioco originale. Per segnalare problemi conservare i log di entrambi i PC e specificare build, host/client, mappa e uso della VPN.

## Sorgenti e compilazione

Sorgenti corrispondenti inclusi in `PenumbraOnlineCoop-source.zip`, licenza GPL-3.0-or-later. `upstream.json` identifica le revisioni ufficiali; `THIRD_PARTY.md` descrive dipendenze e modifiche. `BUILD.md` contiene le istruzioni di compilazione e `TESTS.md` le verifiche svolte.
