# Penumbra: Overture — Online Co-op 0.5 sperimentale

Mod per **2 giocatori** su Windows; **3-4 giocatori** in prova (l'host li sceglie nel launcher; provati solo con più istanze sullo stesso PC). Richiede un'installazione originale di **Penumbra: Overture** su ogni PC. La campagna completa resta da validare.

## Novità della 0.5

- **Inventario per giocatore:**
  - torcia, glowstick, armi, taccuino e mappa li hanno tutti;
  - chiavi, oggetti della storia e consumabili restano a chi li raccoglie.
- **Passare un oggetto al compagno:**
  - si sceglie l'oggetto nell'inventario, si mira al compagno entro 2 m e si
    clicca;
  - delle pile (per esempio gli antidolorifici) passa una unità alla volta.
- **Diario personale:**
  - le note entrano nel diario di tutti, ma si aprono solo a chi le
    raccoglie, e gli altri vedono un avviso;
  - ognuno vede in rosso le note che non ha ancora letto, anche dopo un
    salvataggio;
  - il taccuino aperto non viene più chiuso da aggiornamenti o messaggi.
- **Schede video Intel HD:** corretto lo schermo bianco (bloom) e
  l'immagine sovraesposta con la qualità shader "Alta".
- **Prime prove automatiche fra due PC reali** collegati con Radmin VPN:
  `full`, `hammer`, `campaign` e `resume` passano su entrambi.
- **Runtime Visual C++ x86 incluso** in `bin`: non serve più installarlo.

## Installazione e connessione

1. Estrarre lo ZIP in una cartella separata e aprire **PenumbraCoopLauncher.exe**.
2. Selezionare la cartella del gioco, per esempio `E:\Steam\steamapps\common\Penumbra Overture`. Il launcher trova anche `redist`.
3. L'host sceglie quanti giocatori (2-4) e poi **Ospita partita**. Il client inserisce l'IP dell'host e sceglie **Connettiti all'host**. Porta predefinita: **UDP 27120**.
4. Aggiornare entrambi alla stessa build (**0.5, protocollo 8**): le build precedenti non sono compatibili.

Per giocare non serve PowerShell. Non vengono sostituiti gli eseguibili originali. Il pacchetto non contiene mappe, texture, audio o DLL commerciali: usa quelle della propria installazione `redist`. Il runtime Visual C++ x86 (`msvcp140.dll`, `vcruntime140.dll`) è incluso in `bin`. Se manca OpenAL, installare quello fornito con il gioco.

Su LAN usare l'IP locale dell'host. Su Radmin/Hamachi entrare nella stessa rete VPN e usare l'IP VPN dell'host; consentire `bin/PenumbraCoop.exe` nel firewall. Su LAN/VPN normalmente non serve inoltrare porte sul router. Per collegamento diretto da Internet rendere raggiungibile UDP 27120. Le prove automatiche usano due processi sullo stesso PC, anche con ritardo e perdita simulati, e sono state ripetute fra due PC reali via Radmin VPN; una partita completa giocata a mano fra due PC resta da fare.

## Giocare e combattere

I controlli sono quelli originali e si configurano nelle opzioni. Ogni giocatore ha il proprio inventario: torcia, glowstick, armi, taccuino e mappa li hanno tutti, il resto resta a chi lo raccoglie e si può passare al compagno; il martello e la torcia si equipaggiano separatamente. Il colpo del client inizia subito nella sua visuale, mentre l'host decide danni e rotture. La torcia del partner ha un modello visibile e proietta luce su entrambi i PC.

L'host esegue l'IA originale dei nemici e valuta entrambi i giocatori, con controlli di visuale, distanza, ombra e accovacciamento. Il client riceve animazioni, posizione, salute e corpi dei nemici; non esegue una seconda IA indipendente. I danni degli attacchi possono colpire entrambi. Il verme mantiene l'inseguimento continuo previsto dal gioco originale.

Se uno muore, il compagno può continuare; dopo **5 secondi** viene cercata una posizione libera vicino al superstite per il respawn. Se muoiono entrambi, si carica l'ultimo checkpoint co-op valido. Senza checkpoint valido si tenta il rientro nella mappa corrente. Nascondersi continua a dipendere dalle regole dell'IA originale: non garantisce l'immunità durante un inseguimento.

Gli stati dei giocatori sono inviati fino a 60 Hz, quelli del mondo fino a 30 Hz, entro la frequenza effettiva del gioco. La prediction riduce l'attesa del proprio movimento; danni, porte e altri esiti autorevoli richiedono comunque il passaggio attraverso la rete.

## Salvare e riprendere

- Nel menu di pausa scegliere **Salva checkpoint co-op**. Anche il client può richiederlo, ma il file viene scritto **sul PC dell'host**.
- I punti di autosalvataggio originali dell'host aggiornano anche il checkpoint co-op.
- Prima di uscire salvare e attendere la conferma. L'uscita dal gioco da sola non crea un nuovo checkpoint.
- Per continuare in una nuova sessione l'host seleziona **Host: riprendi l'ultimo checkpoint cooperativo** nel launcher. Il compagno si connette normalmente.
- Durante la sessione l'host può scegliere **Riprendi checkpoint co-op** nel menu di pausa. Ripristina i progressi salvati per entrambi.

Il checkpoint conserva mondo e mappe visitate secondo il salvataggio nativo, variabili e timer della campagna, l'inventario di ogni giocatore, notebook e note lette da ciascuno, salute, batteria, posizione ed equipaggiamento dei giocatori. Gli oggetti trattenuti vengono rilasciati prima del salvataggio; la presa fisica non viene ripristinata.

File normale dell'host:

```
%LocalAppData%\PenumbraOnlineCoop\host\PenumbraOnlineCoop\Overture\save\coop_checkpoint.sav
```

La scrittura usa un file temporaneo e sostituzione atomica. Un controllo d'integrità rileva alterazioni accidentali prima di azzerare il mondo corrente. I vecchi salvataggi senza stato del secondo giocatore non sono compatibili. Conservare una copia del checkpoint prima di aggiornare o fare prove lunghe. Il client non ha una copia autonoma utilizzabile per la migrazione dell'host.

## Steam facoltativo

**Usa Steam** registra il processo come **Penumbra: Overture, App ID 22180**. Serve Steam aperto con l'account che possiede Overture. Per DVD o altre edizioni lasciare l'opzione disattivata.

Il launcher cerca un runtime Valve originale a 32 bit già installato nelle librerie Steam, e il gioco ne verifica la firma. Se il PC non ne dispone, disattivare l'opzione. Nessuna DLL Steam è redistribuita. Non sono implementati inviti Steam o matchmaking.

## Limiti e log

Verifiche e fixture sono descritte in `TESTS.md`; non costituiscono una certificazione di tutta la campagna. Restano da verificare tutte le arene, il nascondersi nelle situazioni reali, ogni arma e consumabile, tutti gli enigmi e gli eventi audio/visivi, e una partita giocata a mano su due PC. Avatar del partner provvisorio; nessuna migrazione host; gli oggetti non si possono ancora lasciare a terra.

**Apri log della mod** apre `%LocalAppData%\PenumbraOnlineCoop`. I log sono in `host\PenumbraOnlineCoop\Overture\hpl.log` e `client\PenumbraOnlineCoop\Overture\hpl.log`. Profili separati dal gioco originale. Per segnalare un problema conservare entrambi i log e specificare build, mappa e rete utilizzata.

Sorgenti corrispondenti inclusi in `PenumbraOnlineCoop-source.zip`, GPL-3.0-or-later. `upstream.json`, `THIRD_PARTY.md` e `BUILD.md` riportano provenienza e istruzioni di compilazione.
