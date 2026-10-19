# Crash e movimento sulle scale — 6 ottobre 2026

## Diagnosi dei log ricevuti

Il log del partner, che in questa sessione ospitava, termina durante
`03_archives.map`, senza chiusura regolare, codice di eccezione o stack.
Il log locale del client mostra la perdita dell'host, il tentativo di migrazione
da un checkpoint completo e infine una chiusura regolare. Questo conferma
la perdita della sessione ma non permette di attribuire il crash alla rete,
alla mod, al codice originale o al driver. L'evento Windows locale del
5 ottobre riguarda una build precedente e non identifica il crash remoto.

Nei log del client ci sono anche intervalli tra snapshot fino a 300 ms in
Archives. Fuori dai caricamenti la replica host riportata nel log del partner
richiede generalmente poche centinaia di microsecondi: non è una misura
del tempo totale di un frame. Neppure la durata complessiva del checkpoint
è un blocco unico: la costruzione è suddivisa in più passaggi.

Gli errori `step_walk_generic_hard.snt` e `step_run_generic_hard.snt` hanno
permesso di individuare un difetto concreto: l'output audio rimaneva non
inizializzato su un caricamento fallito, ma veniva usato per notificare i
nemici. È stato corretto senza attribuirgli il crash non identificato.
I materiali originali Glass, Dyn_Book e altri possono richiedere quel tipo
di passo; se il file manca si usa il suono rock originale corrispondente.

## Correzione e limiti

La previsione orizzontale precedente bloccava il corpo contro alcuni
gradini, trattando spinte verticali superiori a 4 cm come ostacoli. Ora
verifica anche una capsula rialzata entro `GetMaxStepSize()` del motore.
Muri, gradini troppo alti, soffitti e personaggi ostili conservano i controlli
di collisione; il partner resta escluso tramite l'eccezione reciproca.
Il client cerca il sostegno del corpo entro un gradino e accompagna il
cambio di altezza anche quando il numero dello snapshot non cambia.
Salti, cadute oltre un gradino, danni e progresso restano autorevoli sull'host.

La regressione del salto ha inoltre riprodotto una correzione istantanea da
57,1 cm con una conferma tardiva. Il limite precedente di 50 cm cancellava
una differenza compatibile con un salto anticipato. La correzione finale
conserva differenze fino a 1,25 m, riassorbendo quelle sopra 10 cm con una
costante di 100 ms e le più piccole con 40 ms. I salti di posizione autorevoli
restano riconosciuti separatamente. Un nuovo caso unitario verifica il
rientro graduale anche se il decollo non viene mai confermato.

Non cambia il protocollo 11. Usare comunque questa stessa build su entrambi
i PC, perché la gestione dei passi viene eseguita sull'host.

## Prove riproducibili

Build Windows x64 Release, MSVC v145; asset originali Steam locali.
Eseguibili host/client distinti sullo stesso PC, con profili temporanei
dedicati. La scala del test ha otto gradini da 15 cm e un pianerottolo:
sono corpi fisici creati nel mondo nativo, fuori dalla geometria della mappa
e dentro limiti Newton ampliati solo per il test. Non è una percorrenza
manuale delle scale originali né una certificazione della campagna.

| Prova | Risultato |
|---|---|
| Codec/trasporto | 80.000 casi malformati e 20.000 ulteriori casi codec, più handshake, ripresa, caricamento, MTU, perdita e timeout: superati |
| Gradini unitari | Salita/discesa, limite del gradino, muro, soffitto basso, bordo senza sostegno, smoothing con snapshot invariato: superati |
| Due istanze, Entrance Hall | Client: apice 1,193 m, altezza finale −7 mm, massimo cambio verticale 5,1 cm, massimo ritorno all'indietro 0 m; host: apice 1,211 m |
| Due istanze, Archives, relay, build finale | Client: apice 1,194 m, altezza finale −6 mm, massimo cambio verticale 3,2 cm, massimo ritorno all'indietro 0 m; host: apice 1,211 m |
| Suono assente intenzionale | Output azzerato e puntatore nullo, senza uso di dati indeterminati: superato |
| Crash controllato in processo di test | Codice 0xe0424242, testo e minidump scritti; stack letto con CDB: superato |
| Salto/effetti con relay, build finale | Client: apice 1,038 m, atterraggio 0 mm, massimo cambio verticale 11,4 cm; host: apice 1,097 m, spostamento da contatto 0 m; flash, tre visioni e divieto script del salto: superati |

Il relay aggiunge **35 ms per direzione** e scarta un datagramma su 20:
7.529 ricevuti, 7.153 inoltrati, 376 scartati, massimo 1.200 byte.
Il RTT ENet misurato dopo il caricamento è circa 120–140 ms, comprendendo
anche il servizio di rete dei processi. Nella prova finale il client riceve
gli stati in media ogni 19–20 ms durante il movimento e ogni 18 ms dopo,
con massimi di 54–56 ms durante il movimento. La replica client riporta
un massimo di 7,45 ms nel primo intervallo, comprendente il primo caricamento
di `joint_cabinet.snt` da 7,25 ms, poi 1,51 ms nel successivo e valori inferiori
al millisecondo a riposo. Il caricamento iniziale della mappa è separato
e resta sincrono: questa correzione non elimina ogni possibile micro-scatto.

Comandi dalla radice del repository:

```powershell
tests/network/RunTransportTests.ps1 -Port 27190
tests/network/RunCrashDiagnosticsTest.ps1
build/network/NetworkTests.exe --prediction-test
tests/network/RunGameSmoke.ps1 -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' -Stairs -StartMap 02_entrance_hall.map -Port 27205
tests/network/RunImpairedNetworkTest.ps1 -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' -Stairs -StartMap 03_archives.map -Port 27210 -OneWayDelayMs 35 -DropEvery 20
tests/network/RunImpairedNetworkTest.ps1 -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' -Effects -StartMap 04_wine_cellar_lab.map -Port 27220 -OneWayDelayMs 35 -DropEvery 20
```

Evidenze locali in `build/crash-stairs-20261006`: log originali conservati,
`local-stairs`, `impaired-stairs`, `transport-tests-final.log`,
`final-stairs`, `final-effects`, `prediction-final.log`, `crash-diagnostics-final.log` e
analisi del dump di prova. `late-jump-regression` conserva lo scatto rilevato
prima della correzione finale. Il primo tentativo
del test delle scale era fuori dai limiti Newton: è stato conservato come
`initial-stairs`, corretto e ripetuto. Un tentativo dei test di trasporto
usava una porta del gioco contemporaneamente: il percorso definitivo usa
porte separate ed è passato.

## Raccolta dei prossimi crash

Il gestore Windows, installato dopo la scelta della cartella del profilo,
scrive `coop-crash-*.txt` e `coop-crash-*.dmp` accanto a `hpl.log`.
Carica DbgHelp prima dell'eccezione, non accede agli oggetti del motore nel
gestore e mantiene la segnalazione errori Windows. Il dump contiene gli
stack dei thread, non una copia completa della memoria del gioco.
Il pacchetto conserva `symbols/Lux.pdb` e il relativo SHA-256 nel manifest,
per interpretare il dump della stessa build. Non recupera crash precedenti
e può mancare in caso di terminazione forzata o arresti che impediscono al
gestore di eseguire. Per il crash già ricevuto servono l'evento Windows
del PC del partner o un dump già presente su quel PC.

Una partita su due PC tramite Radmin e la riproduzione dell'interazione
che ha preceduto il crash del partner restano da verificare.

SHA-256 dell'eseguibile finale:
`5D85E8F3D6D82C8C215EB73B33889EE750D617E7BF1D78545A878EC292FBC4D5`.
