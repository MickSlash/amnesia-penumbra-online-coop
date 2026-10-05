# Amnesia online: ricompensa degli obiettivi e pietre della memoria

## Segnalazione e diagnosi

Il giocatore client non vedeva il flash blu quando un obiettivo assegnava
il recupero di sanità. Dopo aver interagito con un oggetto compariva inoltre
una schermata bianca con testo e non si riusciva a proseguire.

Il log locale del client, letto da
`Documenti/Amnesia/AmnesiaOnlineCoop_Host/hpl.log` prima delle prove, contiene
`Replica presentation kind=20 ... asset=EmotionStone`. Il traffico prosegue:
questa sequenza è coerente con la visione originale della pietra della memoria.
Il log non registra il clic né basta da solo a dimostrare il punto esatto
del blocco. I due difetti seguenti sono stati verificati nel codice:

- `StartForPlayer` del flash di sanità non inviava un evento online al client.
- Il clic del client per avanzare il testo modificava soltanto il timer locale.
  L'host scartava le azioni del partner inattivo, mentre una visione rende
  intenzionalmente inattivo chi la sta leggendo. Il filtro del lettore poteva
  anche respingere il clic dell'host quando la pietra era stata usata dal partner.

## Correzioni, protocollo 11

Il nuovo evento affidabile `SanityGain` avvia sul client lo stesso effetto
originale, con colore, durata e suono del gioco. La destinazione distingue i
due giocatori; eventi già in corso non riavviano il flash. Le regole originali
della ricompensa restano valide: `GiveSanityBoost` deve essere previsto dallo
script e `AddSanity` non assegna un incremento positivo se la sanità è già 100.

Durante una visione il clic del client viene inviato come intento affidabile.
L'host accetta soltanto l'avanzamento del testo prima del filtro che esclude
le normali azioni dei personaggi inattivi. Il filtro su mappa, epoca, morte,
caricamento e intro resta valido. Entrambi i lettori online possono proseguire;
la proprietà individuale dello schermo in split-screen resta valida.

Scadenza naturale e clic usano lo stesso percorso di chiusura. L'host invia
la fine della visione; entrambe le copie riattivano i comandi e riportano
gradualmente il campo visivo e il blur ai valori originali. Il protocollo
valida la fase Emotion (0 avvio, 1 chiusura). Entrambi i PC devono aggiornarsi.

## Prove native e riproduzione

Visual Studio 2026 / MSVC v145, build Release x64, dati originali di Amnesia.
Due processi reali host/client sullo stesso PC, a 640x480, profili temporanei
separati dal profilo della partita dell'utente.

La mappa è `maps/main/ch01/04_wine_cellar_lab.map`. La prova usa la pietra
originale `emotion_stone_1` (ID 3643), testo `EmotionStone/Alexander01` e
suono `ui_emotion_stone.snt`, attraverso il suo callback nativo `OnInteract`.
Non modifica mappe, script o entità della distribuzione originale.

La fixture esegue la funzione script originale `GiveSanityBoost`, con sanità
iniziale 40 su entrambi, e verifica l'effetto sul client. Il marcatore del
rendering viene emesso da `OnDraw` sul GUI locale con alpha superiore a 0,05.
Poi verifica, in ordine:

1. Pietra attivata come giocatore remoto, clic del client durante l'inattività,
   avanzamento sull'host e ripristino dei comandi autorevoli del partner.
2. Pietra attivata dall'host e testo avanzato dall'host su entrambe le copie.
3. Visione con una breve traduzione originale lasciata scadere senza clic.
4. Comandi attivi, pausa disattivata, FOV assestato entro 0,01 radianti dal
   FOV base e movimento del client di almeno 5 cm dopo la lettura. Il valore
   della prima telecamera può essere alterato dal colpo di sanità dello script
   della mappa: la destinazione del ripristino è il FOV base del giocatore.

La fixture continua a inviare gli input a riposo anche durante e dopo la
lettura, come il percorso di gioco normale. Attende l'assestamento del FOV:
la dissolvenza originale del moltiplicatore è esponenziale e prosegue dopo
la scomparsa della pagina bianca.

La stessa prova sovrappone poi i due personaggi sull'host e fa attraversare
il partner al client, seguito da un salto in movimento. Verifica che le
eccezioni di collisione siano reciproche, che l'host resti fermo, che il salto
compaia sul client prima della conferma e che entrambi vedano salita e
atterraggio. Misura il massimo spostamento verticale per tick, con limite
di 20 cm nella prova. Infine l'host disabilita il salto come uno script della
campagna: il client deve rispettare il divieto anche per l'anticipazione locale.
La fase del salto inizia dopo la conferma della nuova posizione di sovrapposizione:
il teletrasporto intenzionale della fixture viene misurato separatamente dal
salto. Anche un teletrasporto orizzontale azzera la previsione verticale, se
lo spostamento verticale associato è inferiore alla soglia del solo asse Y.

## Salto e collisioni online

Prima, il bit salto interrompeva la previsione orizzontale e l'altezza locale
seguiva ogni nuovo snapshot senza previsione o riassorbimento delle correzioni.
Inoltre la gestione delle eccezioni tra i corpi era attiva soltanto per il
co-op split-screen: i due corpi online potevano quindi bloccarsi reciprocamente.

Il protocollo 11 aggiunge velocità/accelerazione verticali, impulso disponibile
e contatto col suolo agli snapshot e alle notifiche di vita (137 byte).
La pressione e il rilascio del salto partono subito, con un intento affidabile
che conserva anche pressioni brevi fra due input o pacchetti ordinari persi.
Il client mantiene la previsione orizzontale in salto e anticipa una partenza
permessa dal terreno e dall'host per un massimo di 250 ms. Le pose successive
confermano o correggono la salita; l'extrapolazione ordinaria è limitata a 50 ms.
Piccole correzioni verticali si riassorbono su 40 ms. Un controllo della forma
del personaggio in passi di massimo 4 cm mantiene pavimento e soffitto.
Morte, intro, controlli bloccati, cambi mappa e teletrasporti azzerano la previsione.

L'host resta responsabile di salto, atterraggio, danni e conseguenze della storia.
Permessi degli script, collasso di sanità e stato del personaggio determinano
se l'impulso anticipato è disponibile. Online entrambi i corpi ignorano soltanto
quello del partner; i proxy di blocco e le piattaforme sulla testa sono spenti.
Le opzioni di collisione della modalità split-screen conservano il proprio
comportamento; muri, pavimenti, oggetti e nemici mantengono la fisica originale.

```powershell
& tests/network/RunTransportTests.ps1 -Port 27173
& tests/network/RunGameSmoke.ps1 `
  -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' `
  -Effects -StartMap '04_wine_cellar_lab.map' -Port 27170
& tests/network/RunImpairedNetworkTest.ps1 `
  -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' `
  -Effects -StartMap '04_wine_cellar_lab.map' -Port 27177 `
  -OneWayDelayMs 35 -DropEvery 20
```

Il relay aggiunge 35 ms per direzione e scarta un datagramma ogni 20.
L'RTT effettivo comprende anche il servizio dei processi e le ritrasmissioni;
non coincide necessariamente con i soli 70 ms aggiunti dal relay.

## Risultati della build consegnata

SHA-256 di `Lux.exe`:
`8D5830CEDB977B2D979CDCCAC48E5F26C95778BB99DD88E615D90E7B5CB175B8`.

- Test di protocollo e trasporto: passati, compresi 80.000 casi di dati
  malformati e 20.000 ulteriori casi dei codec. Le nuove verifiche includono
  campi verticali, anticipo del salto, conferma, atterraggio, soffitto,
  scadenza dell'anticipo, morte e teletrasporto su più assi.
- Coppia locale: passati flash sul viewport client, tre chiusure della visione,
  ripristino FOV/comandi, movimento di 1,978 m, salita prima della conferma,
  atterraggio, attraversamento del partner e salto vietato dagli script.
  Salto client: apice 1,082 m, scarto all'atterraggio 1 mm, massimo passo
  verticale 9,7 cm per aggiornamento. Spostamento dell'host al contatto: 0 m.
- Coppia attraverso il relay: tutte le stesse verifiche passate. Movimento
  dopo la visione 1,982 m; apice del salto client 1,082 m; scarto
  all'atterraggio 0 mm; massimo passo verticale 17,5 cm per aggiornamento.
  Spostamento dell'host al contatto: 0 m. Il controllo conserva il limite
  di 20 cm; la prova non dimostra l'assenza di ogni correzione visibile.
- Relay: 9.228 datagrammi ricevuti, 8.767 inoltrati, 461 scartati (circa 5%),
  2.781.975 byte inoltrati, 35 ms aggiunti per direzione; nessun datagramma
  scartato per dimensione e massimo inoltrato 1.200 byte.

Log conservati nel workspace: `build/effects-qa/final-local/{host,client}.log`,
`build/impaired-network/{host,client,relay}.log` e
`build/coop-final-protocol-tests.log`. Profili dell'ultima coppia locale:
`OnlineCoopSmoke_20261005195642_118d569d_{host,client}`; della coppia con relay:
`OnlineCoopSmoke_20261005195759_09f79afc_{host,client}`.
Entrambe le coppie terminano con codice 0 e chiusura ordinata del motore.
Il profilo della partita dell'utente è conservato.

## Limiti

La prova automatica attiva i callback originali e il percorso di input della
visione; non percorre manualmente tutta la mappa e non completa la campagna.
Il controllo del flash riguarda il percorso nativo di disegno, non una
comparazione dei pixel o un ascolto umano. Una partita su due PC tramite
Radmin VPN, con le configurazioni dei due giocatori, richiede ancora una prova.
Questa correzione riguarda effetti e lettura; la fluidità complessiva della
rete dipende anche da ritardo, perdita di pacchetti e prestazioni dei due PC.
