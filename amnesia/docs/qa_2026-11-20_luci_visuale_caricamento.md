# QA 20 novembre 2026 — luci dell'Orb Chamber, visuale del client, schermata di caricamento (0.6.3, protocollo 19)

## 1. Sfarfallio sul client nello scontro finale (29_orb_chamber)

Segnalazione: video del client durante lo scontro finale. Una luce calda vicino
alla telecamera si accende per un fotogramma ogni 3-4 (15-20 volte al secondo).
Le lanterne di entrambi i giocatori erano spente, l'host non vede il difetto e
con la 0.5 la campagna era stata finita senza.

### Riproduzione (MISURATO)

`RunGameSmoke.ps1 -MapOnly -StartMap 29_orb_chamber.map -Visual`. Durante la
prova si cattura la finestra del client con PrintWindow, circa 55 catture al
secondo. Per ogni cattura si calcola la luminosità media di una griglia 4x4.

| Finestra | Sbalzi ≥ 6 livelli fra catture consecutive (120 catture, 16 celle) |
| --- | --- |
| Client, 0.6.2 | circa 1000: sequenza scuro-chiaro-chiaro su tutta l'immagine |
| Host, 0.6.2 | 3 |
| Client, dopo la correzione | 9 (variazioni graduali della scena) |

### Causa

Una diagnostica temporanea, ora rimossa, registrava per ogni fotogramma le luci
del client che cambiavano colore. Ha mostrato che PointLight_1, 14 e 33 cambiavano
più spesso di quanto arrivassero aggiornamenti dall'host.

Il file della mappa originale ha **più luci con lo stesso nome**: 22 nomi doppi,
per esempio tre "PointLight_1", tra cui una bianca di raggio 1,5 e una calda di
raggio 5. È l'unica mappa della campagna con questo problema.

L'host inviava le luci per nome:
- il client applicava i due o tre stati alla prima luce con quel nome, che
  saltava fra colori e raggi diversi;
- l'host confrontava gli stati di luci diverse sotto lo stesso nome e li
  rispediva a ogni tick, occupando il limite di 64 luci per tick.

### Correzione

Dalla seconda luce in poi con lo stesso nome, l'identificatore è `nome#n`, in
ordine di caricamento. L'ordine è uguale su host e client perché caricano la
stessa mappa. Il client risolve `nome#n` cercando la n-esima luce con quel nome.

## 2. Visuale del client: accucciarsi e LookAt degli script

Per il giocatore locale il client prendeva altezza degli occhi, scostamenti
della testa, rotazione, FOV e, con la visuale guidata dall'host, gli angoli
dall'**ultimo snapshot arrivato**. La versione interpolata sulla timeline
dell'host veniva calcolata e poi ignorata. Con l'arrivo irregolare dei pacchetti
la visuale restava ferma un fotogramma e poi faceva un salto doppio: durante il
crouch, nelle dissolvenze del FOV e nei `StartPlayerLookAt`.

Correzioni:
- Altezza degli occhi, scostamenti della testa, rotazione, inclinazione
  dell'host, FOV e proporzioni del giocatore locale vengono dalla timeline
  interpolata. L'interpolazione ora copre anche `leanRoll`, `fov` e `aspect`.
  Il lean locale (0.6.1) resta: la parte dell'host si toglie con valori dello
  stesso istante.
- Con LookAt, visuale bloccata o guidata da un oggetto (flag 16/32, o
  giocatore inattivo), anche yaw e pitch vengono dalla timeline. Durante un
  LookAt (previsione del movimento spenta) lo stesso vale per la posizione.
- Durante un LookAt il mouse non ruota più la visuale locale, che lo snapshot
  riportava indietro. Il movimento del mouse va comunque all'host, come nel
  gioco originale.
- Previsione: il tasto Accovacciati alterna lo stato previsto e cambia subito
  la velocità massima (da accovacciati la corsa non conta, come in
  `UpdateSpeedMultipliers`). La capsula dell'ultimo snapshot viene impostata
  prima della previsione, non un fotogramma dopo.

Effetto: per il crouch e per i LookAt la visuale segue l'host con circa 100 ms
di ritardo in più, la stessa timeline del compagno, ma senza scatti.

Non toccato: il recupero dei comandi sull'host (`CatchUpInput`) non aggiorna
la testa nel tick extra. La curva della testa avanza comunque a ogni fotogramma
e sul client l'interpolazione la rende continua.

## 3. Schermata di caricamento con la citazione anche sul client

Prima il client vedeva solo "L'host sta caricando la mappa...". Ora:

1. **Host, cambio mappa:** dopo `DrawGameScreen` manda `LoadScreen` (tipo 1)
   con categoria della lingua, voce già scelta e immagine. La scelta casuale e
   il registro delle frasi già lette restano sull'host, quindi i due giocatori
   leggono la stessa frase, ciascuno nella propria lingua.
2. **Client:** mostra la stessa schermata (contenitore `LoadScreen`) e ci
   carica sotto la mappa replicata, senza tornare al contenitore `Default`.
3. **Host, dopo il proprio caricamento:** il tempo di lettura della citazione
   scorre. La schermata resta finché il client non ha applicato il nuovo mondo
   (barriera di caricamento), anche se l'host preme un tasto.
4. **Chiusura:** quando l'host inizia la dissolvenza manda `LoadScreen`
   (tipo 2) e il client fa la stessa dissolvenza. Il suono della porta del
   client (suono d'arrivo della 0.6.2) passa alla fine della schermata, una
   volta sola.

Il client esce comunque se la connessione cade, o dopo 2 minuti senza la
chiusura. Al primo avvio della campagna e nei caricamenti di un salvataggio
non c'è schermata condivisa (l'host non chiama `DrawGameScreen`).

### Prova (MISURATO)

`RunGameSmoke.ps1 -LevelDoor -Visual`. L'host imposta una citazione vera della
campagna (`Ch01_Diary01_`, 6 frasi, `game_loading_catacombs.jpg`) e passa per
una porta di livello verso 09_back_hall.

- Host: `Load screen shared: 1 entry=Ch01_Diary01_04`, poi la barriera
  rilasciata, poi `Load screen closed on both players sent=1`.
- Client: `Shared load screen shown entry=Ch01_Diary01_04`, mondo applicato,
  `Arrival sound kept for the end of the load screen: door_level_wood_close`,
  `Host closed the shared load screen`, `Shared load screen closed released=1`.
- Catture del client ogni 0,4 s:
  1. "Waiting for the host" durante il blocco di 4 s dell'host;
  2. la citazione inglese sull'immagine, tenuta per 22 catture;
  3. la dissolvenza nella nuova mappa.

`RunGameSmoke.ps1` controlla queste righe nella prova `-LevelDoor`.
`NetworkTests` verifica andata e ritorno di `LoadScreen` e il rifiuto di
pacchetti troncati, immagini fuori dal gioco o con percorso assoluto, e tipi
non validi.

## 4. Suite completa (MISURATO)

21 prove di gioco su 21 PASS, contate per parola di stato:
- trasporto e diagnostica dei crash;
- campagna base, con rete degradata e al cancello dell'archivio;
- giocatore dell'host, arresto del movimento, ripresa dopo disconnessione;
- avvio della campagna con l'intro;
- nemici, scale con rete degradata, effetti;
- leve, anche con jitter, e flashback;
- inclinazione Q/E normale e con rete degradata;
- replicazione dei giocatori, porta di livello, rientro con Steam;
- previsione con jitter.

`NetworkTests`: tutti PASS.

## 5. Non verificato

- Partita reale su due PC con la 0.6.3, e lo scontro finale giocato fino in fondo.
- Crouch e LookAt valutati a occhio su una rete reale: le prove automatiche
  coprono il codice, non la sensazione.
