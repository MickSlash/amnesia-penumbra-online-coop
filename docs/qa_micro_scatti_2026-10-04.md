# Riduzione dei picchi sul thread del gioco — 4 ottobre 2026

## Intervento

La costruzione XML del checkpoint host avveniva interamente in un aggiornamento.
Ora viene avanzata sul thread del gioco in passaggi con un budget cooperativo di
1,5 ms. Il salvataggio della mappa corrente viene catturato una sola volta e
possiede i propri dati. Le mappe storiche sono lette solo finché puntatore,
revisione, epoca e sessione rimangono validi. Un cambio mappa, reset o cambio
sessione annulla la cattura; nessun iteratore obsoleto viene avanzato.
Anche i reset usati dal caricamento salvataggi e dal test debug delle mappe
passano dal metodo che invalida la revisione.
Stampa, compressione, hash e distruzione del grande albero XML restano sul worker,
senza puntatori al mondo del motore. Il checkpoint valido precedente resta
disponibile durante il lavoro nuovo.

I buffer temporanei del formatter nativo sono separati per thread. Il thread
degli autosalvataggi su disco può formattare numeri e testo Unicode mentre il
thread del gioco serializza un checkpoint, senza sovrascrivere quei buffer.
Il formato del salvataggio e il protocollo 7 rimangono invariati.

Il client non esegue OnEnter, perché contiene anche modifiche alla campagna.
Questo faceva saltare i precaricamenti di suoni e particelle presenti nello
script originale. Ora vengono estratte solo chiamate con un argomento letterale
a PreloadSound/PreloadParticleSystem, ignorando commenti, stringhe e argomenti
dinamici. Si precaricano le risorse durante il caricamento, prima di MapReady.
Il lettore limita sorgente, numero di risorse e percorsi. Le code di replica di
media, presentazione ed entità hanno inoltre budget cooperativi di 0,75 ms per
gruppo; gli eventi conservano l'ordine e rimangono in coda per i frame seguenti.
La baseline viene confermata solo dopo il drenaggio delle code del mondo.

Durante la verifica è emerso anche un conflitto di lifecycle: dopo la rimozione
di una particella da parte della replica, SetEffectsActive poteva usare il
puntatore conservato dal suo prop. DestroyParticleSystem dereferenziava e
eliminava quel puntatore anche se non apparteneva più al mondo. Ora controlla
l'appartenenza prima di accedervi. I prop del client rilasciano i loro puntatori
audio/particelle alla replica e non creano, distruggono o riavviano quei media
tramite le normali transizioni locali; mantengono mesh, luci e flag visivi.
Il flicker locale viene disattivato quando si applicano gli effetti autorevoli.

La prova di oscuramento degli snapshot ordinari ha inoltre mostrato che un
comando affidabile perso sul canale di controllo poteva trattenere anche gli
stati di riserva affidabili successivi. Ogni riserva a 10 Hz mantiene ora la
copia affidabile e aggiunge una copia ENet unsequenced dello stesso stato.
Questa supera il buco del canale; epoca e sequenza applicative respingono copie
vecchie e duplicate. Le transizioni vita e gli eventi di campagna restano
ordinati, su messaggi separati. La duplicazione aggiunge circa 1,2 KiB/s di
contenuto prima dell'overhead UDP, senza cambiare formato o protocollo.

La prediction riutilizza inoltre i risultati delle collisioni agli stessi
identici punti all'interno di un singolo aggiornamento: il mondo, la forma e
l'altezza rimangono invariati durante quel blocco di codice. La cache contiene
al massimo 128 risultati e viene ricreata a ogni aggiornamento, quindi una
porta o un oggetto spostati vengono verificati nuovamente. Non arrotonda le
coordinate e non omette controlli per movimenti reali; evita anche le query
degli assi senza spostamento. Le operazioni di prediction superiori a 5 ms
registrano durata, numero di query native e numero di risultati riutilizzati.

## Confronto nativo

Build finale:
`BDAD3AAFC78082BB35652DD6EE95C5369165C1E4A0A12F76C88EB22DAC357776`.
Prova di 210 s: `build/level-network-probe/20261004_131553_4e827866`,
10 checkpoint validati, 10.796 stati client, una sola handshake per processo,
nessun crash o errore di sincronizzazione.

| Massimo osservato | Build precedente | Build finale |
| --- | ---: | ---: |
| Checkpoint host: lavoro in un frame | 19,580 ms | 1,658 ms |
| Replica client, esclusa la prima finestra di caricamento | 28,344 ms | 3,574 ms |
| Intervallo fra stati client registrati | 67 ms | 50 ms |

Le condizioni di rete e grafica coincidono, ma la copertura del percorso varia:
la build finale raggiunge i punti 0, 1, 2 e 5; 3 e 4 sono ostacolati da Door_1,
i successivi dalla geometria del corridoio. Non entra nella stanza laterale
raggiunta dalla prova precedente. I massimi descrivono quindi queste sessioni
campionate, senza attribuire l'intera differenza di ogni massimo alla sola
ottimizzazione. Le finestre tipiche della replica client durante il movimento
sono circa 0,5–1,5 ms medi, rispetto a circa 4 ms nella prova FE88 successiva
alla prima ottimizzazione. Il caricamento iniziale resta circa 1,38 s.
Il relay riceve 24.568 datagrammi, ne inoltra 23.340 e ne scarta 1.228; nessun
datagramma supera 1.200 byte. Il livello non viene completato.

La stessa build supera anche la prova combinata di media/prediction/blackout,
salvataggio e caricamento, cambio livello, morte e ripristino condivisi:
`build/microstutter-qa/media-BDAD` e
`build/microstutter-qa/media-final-validation.log`. Massimo intervallo registrato
249 ms nella finestra del blackout/salvataggio; nessun gap superiore a 250 ms
segnalato. Sono mantenute la prediction prima dell'ACK e le correzioni graduali
con collisioni. Il test di doppia distruzione della particella passa.
Relay: 6.380 ricevuti, 6.061 inoltrati e 319 scartati; massimo 1.200 byte.

La migrazione della build finale passa con confronto XML esatto, collezione di
mappe storiche popolata, formattazione concorrente, worker ritardato di 350 ms e
risultato obsoleto da scartare. Sono ripristinati la variabile di campagna, gli
stati individuali scambiati e la cronologia del journal. I processi terminano
correttamente. Log: `build/microstutter-qa/migration-BDAD` e
`build/microstutter-qa/migration-final-validation.log`.

Due istanze reali del gioco sullo stesso PC, profili QA nuovi, Old Archives
avviato direttamente, 960×720, texture/ombre medie, SSAO disattivato, limite
60 Hz. Un relay UDP aggiunge 50 ms per direzione e scarta un datagramma ogni 20.
Non è una prova sui due PC collegati attraverso Radmin.

La base del confronto è il binario
`A3D64CA69A63ED6FA3BAD87A24E61D3D343EAE9655ED03D838E372B30B84EEFD`,
prova `build/level-network-probe/20261004_110604_aaad419b`, descritta in
`docs/qa_old_archives_rete_2026-10-04.md`.

Prima prova dell'ottimizzazione, prima dell'isolamento dei buffer per thread:
binario `063503AA22C90FE55C886026F75DBCDF246096B19CBC0C4A49227C24F1C15FC2`,
prova `build/level-network-probe/20261004_122239_ff289359`, 210 secondi.

| Misura | Build precedente | Prima prova ottimizzata |
| --- | ---: | ---: |
| Checkpoint host: massimo lavoro in un frame | 19,580 ms | 1,763 ms |
| Replica client: massimo dopo la prima finestra di 5 s | 28,344 ms | 9,255 ms |
| Checkpoint validati dal client | 10 | 10 |
| Stati client registrati | 10.996 | 11.278 |
| Massimo intervallo fra stati registrati | 67 ms | 100 ms |

Il massimo intervallo di rete non è migliorato in questa prova: perdita e
ritrasmissioni restano variabili e non sono limitate dal budget CPU. Le medie
delle finestre vanno verificate separatamente dai picchi di lavoro del motore.
Nel caricamento iniziale il nuovo client impiega circa 1,65 s nella replica,
compresi 261 ms per precaricare 27 suoni e 7 sistemi particellari. Questa pausa
di caricamento non è inclusa nel massimo durante il gioco della tabella.

Le finestre host/client sono state ispezionate e sono conservati
`host-after-effects.png` e `client-room.png`. Non si tratta di una certificazione
visiva della fluidità: uno screenshot non misura il tempo dei frame.
Il percorso raggiunge i primi cinque punti, attraversando corridoio ed evento
iniziale e entrando nella stanza laterale. I punti successivi risultano
ostacolati dal mobilio; input ordinari aggiuntivi riportano il client verso
l'ingresso della stanza. Nessun teleport o callback di puzzle viene invocato.
L'host rimane fermo, la partita non termina il livello.

## Verifiche di correttezza

- Release x64 compilata; due avvisi C4244 già presenti nel progetto.
- Test di trasporto con AddressSanitizer e 100.000 casi malformati, code,
  identità, sequenze, transizioni vita e snapshot di riserva. Il primo tentativo
  durante la compilazione completa fallisce una verifica temporale su un
  snapshot UDP non affidabile; la suite ripetuta a compilazione terminata passa.
- Test del lettore di precaricamenti: commenti, stringhe, escape, duplicati,
  argomenti dinamici, percorsi non validi, limiti e sorgente troncata.
- Test nativo dell'identità XML fra serializzatore originale e incrementale,
  con mappa corrente e collezione di mappe storiche popolata.
  Con il flag di parità si ricostruisce deliberatamente l'XML originale nello
  stesso aggiornamento: i picchi di quella prova non misurano il comportamento
  ordinario della build distribuita. Verificata anche la formattazione
  contemporanea di vettori e stringhe Unicode, 10.000 iterazioni per thread.
- Test di cattura annullata dopo reset delle mappe storiche, seguito da nuovo
  checkpoint valido e riconnessione; log in
  `build/microstutter-qa/reconnect-cancel`. Massimo per frame osservato 1,649 ms.
- Migrazione reale dei due processi con worker deliberatamente rallentato e
  risultato obsoleto: ripristinati variabile di campagna, stati individuali
  scambiati e cronologia journal; log della prima build in
  `build/microstutter-qa/migration-parity`.

Una successiva prova del binario `ADD0F769D6BA091514F52F6331F4131EFE830CFCFBEE9A6AB4CA567FE60DD612`
in `build/level-network-probe/20261004_123126_f19ac50c` registra un crash client
0xc0000005. Il registro WER e il PDB locale risolvono l'istruzione al distruttore
iEntity3D, durante l'iterazione dei figli. Non si dispone del dump WER completo:
la doppia eliminazione è un percorso scorretto verificato nel codice, coerente
con questa istruzione, senza un'attribuzione definitiva dello stack originale.
La prova con debugger all'avvio altera i tempi di caricamento e provoca
riconnessioni: non è usata per valutare prestazioni. La prova con debugger
collegato dopo il caricamento (`20261004_123846_de25e6a3`, 60 s) passa, ma non
riproduce il crash. Il nuovo test nativo invoca deliberatamente due volte la
distruzione della stessa particella e deve proseguire senza dereferenziare il
puntatore scaduto.

## Verifica dei media e delle code di movimento

Il binario `9935CB3852C7E684D092DC1CD9A7A1434CB7960115823F5C698D0B3D90E9D329`,
con la correzione dei media, completa 210 s in
`build/level-network-probe/20261004_125245_e90fbc70`: massimo checkpoint host
1,588 ms, massimo replica client durante il gioco 12,965 ms e massimo intervallo
fra stati 50 ms, senza crash. La prova combinata media/prediction/blackout
conservata in `build/microstutter-qa/media-final` rileva però un intervallo di
2.249 ms nell'epoca iniziale: il backup affidabile era bloccato da un comando
mancante. Questo risultato ha motivato la copia unsequenced.

Il binario `4AEA028645A3F1C0602654DAED35C4A2C227797E68AB4EFF620EBFFB81039E06`,
con quella copia, supera la prova combinata; log in
`build/microstutter-qa/media-twin-pass`. Sono verificati creazione/modifica/
rimozione di suoni e particelle, musica, effetti globali, lanterne indipendenti,
prediction, salvataggio/caricamento, cambio mappa, morte e ripristino condivisi.
Il massimo degli intervalli registrati è 217 ms nella finestra che comprende il
blackout e il salvataggio. Relay: 6.421 datagrammi ricevuti, 6.100 inoltrati,
321 scartati, nessuno oltre il limite di 1.200 byte.

La prima prova della copia (`build/microstutter-qa/media-twin-transition`)
fallisce l'asserzione generale di 700 ms per una pausa di 1.217 ms nell'epoca 2,
quando l'host carica la mappa seguente per 1.220 ms. Il controllo del blackout
è stato circoscritto alla sua epoca iniziale, mantenendo la soglia di 700 ms;
le pause delle epoche successive vengono riportate separatamente. Quel
caricamento resta sincrono: non viene presentato come una pausa risolta.

Il test di trasporto con AddressSanitizer lascia deliberatamente un buco nella
sequenza affidabile ENet: verifica che lo stato affidabile successivo resti
trattenuto, che la copia unsequenced venga applicata, che una copia vecchia non
riporti indietro la posizione e che non venga generato un evento di vita.
La suite completa passa dopo la correzione del canale della mappa nel server
fittizio del test.

La prova successiva di 210 s del binario
`FE88C6A0AE848FAB8E65A9965B09A6316C645968CD9EFA5063FD22A95A9CD1EE`,
`build/level-network-probe/20261004_130853_20b5d2a1`, mantiene dieci checkpoint
validati e intervalli fra stati non superiori a 50 ms. Il massimo checkpoint
host è 1,558 ms. Il client registra ancora un massimo isolato di 26,603 ms,
con finestre tipiche intorno a 4 ms medi: non è quindi una prova di eliminazione
di ogni picco. Questa misura ha motivato la riduzione delle query di collisione
ripetute nella prediction descritta sopra.

I test della cache verificano il riuso esatto di un contatto, la distinzione
fra un punto libero molto vicino e uno bloccato, l'invalidazione fra
aggiornamenti e il corretto ricorso alle query quando si supera la capacità.
Log della suite finale AddressSanitizer:
`build/microstutter-qa/transport-asan-final.log`.

## Limiti

Il budget è cooperativo: una singola operazione nativa costosa e la sospensione
del thread da parte di Windows possono superarlo. Si distribuisce il lavoro
totale su più frame; non si garantisce che ogni funzione del motore duri meno
di 1,5 ms. Caricamento mappa, decoder musicali, risorse non dichiarate come
preload, collisioni e rendering possono ancora richiedere lavoro sincrono.
Le misure riguardano replica e checkpoint, non il tempo totale CPU/GPU di
ogni frame. Occorre ancora la prova della coppia di PC attraverso Radmin e
dell'intera campagna.

## Riproduzione

Compilazione Release x64 con MSBuild di Visual Studio 2026 e override del
toolset `v145`; il sorgente del progetto conserva il suo toolset precedente.
Le prove native creano profili QA separati e non usano i salvataggi personali.

```powershell
./tests/network/RunTransportTests.ps1 -Executable ./build/network-asan/NetworkTests.exe -Port 27055
./tests/network/RunLevelNetworkProbe.ps1 -GameDirectory 'F:/SteamLibrary/steamapps/common/Amnesia The Dark Descent' -OneWayDelayMs 50 -DropEvery 20 -Seconds 210 -Port 27080
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory 'F:/SteamLibrary/steamapps/common/Amnesia The Dark Descent' -OneWayDelayMs 50 -DropEvery 20 -Port 27036 -MaxDatagramBytes 1200 -Media -Prediction -SnapshotBlackout
./tests/network/RunRecoveryGameTest.ps1 -GameDirectory 'F:/SteamLibrary/steamapps/common/Amnesia The Dark Descent' -Mode migrate -SerializationParity -SlowCheckpoint -StaleCheckpoint -Port 27060
```

Questi comandi servono per riprodurre i test di sviluppo. Per giocare basta il
launcher grafico `AmnesiaCoop.exe` incluso nel pacchetto.
