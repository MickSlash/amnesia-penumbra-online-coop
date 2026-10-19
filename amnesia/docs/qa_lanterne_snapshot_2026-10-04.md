# Lanterne, fluidità del client e code di rete — 4 ottobre 2026

## Evidenza e cause riprodotte

Il giocatore segnala scatti del movimento client con visuale mouse fluida,
lanterna propria non visibile/usabile e luce dell'host assente. Precisa che
il consumo CPU totale è circa 20–30%: questo dato non dimostra saturazione CPU.
Il precedente log client registra RTT di circa 47–62 ms ma intervalli fra stati
di 526–1.732 ms. RTT e frequenza degli stati applicati sono misure distinte.

Le prove locali hanno riprodotto due cause concrete:

- Con 100 ms nominali e perdita deterministica del 5%, la coda ENet con limite
  di 256 KiB si ferma a 262.790 byte. Il ricevitore rifiuta anche il pacchetto
  affidabile mancante che consentirebbe di svuotarla; gli invii si accumulano,
  fino alla disconnessione. Non è una prova della stessa causa sulla rete Radmin
  dell'utente, per cui manca il log host della medesima sessione.
- Anche dopo il recupero della coda, picchi degli ACK possono portare il
  throttle ENet a `0/32`: il trasporto scarta tutti gli snapshot ordinari.
  Una prova registra così gap di 433–1.067 ms durante la replica in Old Archives,
  con RTT di circa 157–162 ms. Il codice ENet incluso conferma questa semantica.

Un'altra prova scopre un checkpoint rifiutato correttamente dal validatore:
`mfImageTrail_FadeSpeed` non finito. ImageTrail, Sepia e RadialBlur non
inizializzavano quel campo prima del primo effetto e non lo azzeravano al reset.

## Correzioni

- Il client aggiorna le trasformazioni delle mani dopo la camera. La lanterna
  impugnata segue la visuale invece di restare nella posizione iniziale.
- La replica autorevole supera permessi locali `Disabled` obsoleti. I normali
  comandi host mantengono controlli su possesso, olio e permessi.
- Snapshot dei giocatori a **60 Hz**; audio, entità e luci restano a **20 Hz**.
  Un backup affidabile da 123 byte arriva sul canale di controllo ogni 100 ms
  e al cambio lanterna. Sequenze ed epoch condivise impediscono riavvolgimenti;
  il backup non genera eventi Life né cancella prediction/interpolazione.
- Ricezione ENet con budget di **4 MiB** e singolo pacchetto massimo 64 KiB.
  Gli invii voluminosi vengono rinviati oltre **32 KiB** affidabili in coda/in
  volo oppure durante ritrasmissioni/ACK attesi da oltre 250 ms. I chiamanti
  conservano lo stato e riprovano; controllo e stati dei giocatori restano
  inviabili. `bulk_paused` espone questa pressione nei log.
- Decelerazione del throttle ENet basata su RTT impostata a zero: i picchi degli
  ACK non sopprimono gli snapshot. Restano i limiti di banda del trasporto,
  le frequenze applicative e il controllo del traffico voluminoso.
- Piccole correzioni orizzontali riassorbite con decadimento di **80 ms** e
  collisioni. Gli input nuovi restano immediati. Scarti superiori a 25 cm,
  cambi verticali, morte e stati speciali cancellano la correzione.
  Prediction limitata a **32 comandi / 500 ms**, senza aggiungere un buffer
  d'attesa degli input: copre brevi gap senza simulare tutta la fisica.
- Velocità di fade degli effetti inizializzate e azzerate al reset, conservando
  il validatore dei checkpoint. La prova nativa inietta deliberatamente valori
  non finiti, esegue il reset e verifica i valori serializzati uguali a zero.
- Log con media/massimo degli intervalli applicati in finestre di cinque
  secondi, compresa la finestra finale; risultato dei comandi lanterna remoti.

Protocollo **6**: aggiornare entrambi i PC. Le build precedenti sono rifiutate.

## Build e verifiche finali

Release x64 MSVC v145, compilazione senza errori o avvisi. SHA-256 di Lux.exe:
`7D0CA90112E2FB8230FA56EEE48D498B4ECEBCE6DEE5822ED31A7D0513CE82D4`.

La suite del trasporto con **AddressSanitizer passa** 100.000 casi deterministici
di input malformati, codec, handshake/versioni, journal, checkpoint, code Life,
riconnessione e inversione dei ruoli. Nuove regressioni provano:

- recupero tramite backup senza snapshot ordinari, pacchetti ritardati e assenza
  di eventi Life impropri;
- rinvio del traffico voluminoso, consegna del controllo e ripresa dopo gli ACK;
- negoziazione reale della politica ENet e 64 picchi RTT consecutivi senza
  azzerare il throttle;
- convergenza delle correzioni, collisioni, teletrasporti, morte, input immediati
  e avanzamento durante oltre 400 ms senza nuovi stati.

Log: `build/lantern-stutter-qa/final-transport.log`. ASAN riguarda il binario
dei test di rete, non l'intero gioco Lux.exe.

La prova nativa usa due processi, profili QA separati e gli asset Steam
installati. Il relay impone il limite di 1.200 byte per datagramma.

### 70 ms nominali, perdita di un datagramma ogni 1.000

**PASS su host e client**: media degli intervalli applicati 17 ms, massimo
50 ms su 3.024 intervalli; nessun gap registrato oltre 250 ms. Il relay riceve
7.220 datagrammi, ne inoltra 7.213 e ne scarta 7; nessuno supera il limite MTU.
Quattro checkpoint validati, fasi principali client di 261–695 µs.
Artefatti: `build/lantern-stutter-qa/final-wan70-throttle`.

Sono verificati movimento con prediction e collisioni, pickup/inventario,
save/load Entrance Hall → Old Archives, morte/ripresa ordinate, audio/effetti
e lanterne indipendenti. La lanterna locale è attiva e segue la camera in
movimento entro 25 cm; entrambi gli helper luminosi client emettono luce anche
con `Disabled` locale iniettato. Il comando remoto viene validato dall'host e
spegnere la lanterna host lascia accesa quella client.

### 100 ms nominali, perdita deterministica del 5%

**PASS su host e client** sulla stessa build finale: media degli intervalli
17–18 ms, massimo 66 ms su 2.856 intervalli; nessun gap registrato oltre 250 ms, disconnessione
durante il test o checkpoint non valido. Il relay riceve 6.416 datagrammi,
ne inoltra 6.096 e ne scarta 320, senza datagrammi oltre 1.200 byte.
Tre checkpoint validati, fasi principali client di 238–516 µs. Il throttle
host resta `32/32`; sei campioni `bulk_paused=1` mostrano che il traffico
voluminoso viene rinviato. La coda di ricezione client misurata ogni cinque
secondi raggiunge 35.542 byte e torna a zero, senza il plateau di 262.790 byte
della regressione precedente. Questi campioni non misurano ogni picco istantaneo.
Passano le medesime integrazioni di campagna, movimento e lanterne.
Artefatti: `build/lantern-stutter-qa/final-loss5-throttle`.

### Arresto del movimento

**PASS** sulla build finale: rilascio dei tasti avanti e laterale arresta
il corpo autorevole e la prediction client: velocità a riposo e deriva misurate
pari a zero su entrambi gli assi, su host e client. Non viene reintrodotta la camminata
continua della precedente segnalazione. Artefatti:
`build/movement-stop/20261003_231359_51056ab6`.

### Riconnessione e checkpoint

**PASS** sulla build finale, con worker rallentato deliberatamente di 350 ms
e risultato invalidato mentre è in elaborazione: il risultato obsoleto viene
scartato, un checkpoint successivo viene validato, la sessione si riconnette
e ricostruisce mondo e cronologia del journal. Passano note, due diari ordinati
e obiettivi attivi/completati su entrambi i processi. Le fasi principali del
checkpoint restano sotto il limite del test di 16 ms anche col worker rallentato.
Artefatti: `build/recovery-qa/reconnect`.

## Prove limite e vincoli

Prima della correzione finale del throttle, una prova con 100 ms nominali,
perdita del 5% e soppressione degli snapshot ordinari per tre secondi **fallisce
la soglia di fluidità di 700 ms**: una ritrasmissione di controllo produce un
gap di 1.133 ms. Si registra inoltre un gap di 1.183 ms durante il caricamento
host, con pausa del servizio di rete di 1.176 ms. La sessione non cade e arriva
a morte/ripresa; il primo checkpoint non valido porta alla correzione dei fade.
Il risultato negativo resta conservato in
`build/lantern-stutter-qa/final60-loss5-blackout-first` e non viene trasformato
in PASS cambiando la soglia. Il test dimostra un limite della riserva affidabile
in presenza di perdita e blackout, non certifica un intervallo massimo.

Le prove delle lanterne verificano helper, comandi e trasformazioni nel motore;
non sono screenshot su due PC remoti. La campagna automatica usa fixture e non
completa i puzzle o l'intera partita. Radmin reale resta da verificare sui PC
del giocatore. Caricamenti del livello, perdita prolungata e salti/crouch possono
ancora produrre pause o correzioni: nessuna promessa di zero scatti in ogni rete.
La prediction riguarda soltanto il movimento orizzontale.

Olio e accensione restano individuali; olio vuoto impedisce la lanterna. Il
partner vede luce e posa, senza un nuovo modello dettagliato in terza persona.
Se persistono scatti, conservare i log di entrambi subito dopo la stessa partita,
prima di riaprire il gioco.
