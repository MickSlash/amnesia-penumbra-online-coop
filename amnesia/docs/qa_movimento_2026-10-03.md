# Correzione della camminata continua del client — 3 ottobre 2026

Segnalazione dopo la build VPN: il client può controllare il personaggio, ma questo continua a camminare senza tasti premuti. Non abbiamo accesso al PC remoto per misurarne direttamente gli input.

## Causa individuata nel codice

`ApplyRemotePlayerInput` chiamava `cLuxPlayer::Move` su entrambi gli assi ad ogni tick, anche con input zero. La chiamata arriva a `iCharacterBody::Move`, che imposta `mbMoving[aDir] = true` indipendentemente dal moltiplicatore. La fisica decelera normalmente solo gli assi senza movimento: dopo un input non nullo, `Move(0)` impediva quindi di ridurre la velocità residua. Inoltre attivava inutilmente `mbPressedMove` sul giocatore.

Ora l'host chiama `Move` solo per gli assi non nulli, come fa il percorso locale dei tasti. La fisica applica la decelerazione originale al rilascio. La stessa correzione è nel driver QA locale, che riproduce gli input normali. Non modifichiamo il motore globale, la velocità di camminata, i controlli configurati o il protocollo 5. Prediction e stato autorevole continuano a usare le velocità del corpo simulate dall'host.

## Prova nativa di arresto

```powershell
./tests/network/RunMovementStopTest.ps1 -GameDirectory GAME_PATH
```

Il test avvia due processi reali con profili separati, carica Entrance Hall, aspetta la baseline, comanda il client sul primo asse, verifica una velocità non nulla sull'host, rilascia entrambi gli assi e controlla l'arresto. Ripete sul movimento laterale. Legge la velocità effettiva del corpo autorevole e le posizioni dell'host e del client; la telemetria aggiunta è disponibile solo con il driver QA esplicitamente attivato. Non forza posizioni o velocità per far passare l'asserzione.

Binario SHA-256 `E482C3298BD65F604CCCA95DAD5C69AB02498830E9638F7F822EAAB65A3ED78D`.

Esito: **PASS**, entrambi i processi terminano sul timer senza errori. Per ciascun asse velocità osservata in movimento 1,6 m/s; a riposo 0 m/s. Nella finestra di un secondo dopo l'assestamento non è stata misurata deriva orizzontale, né sull'host né sul client, alla risoluzione della telemetria di 0,1 mm. Il test richiede velocità sotto 0,01 m/s e deriva sotto 4 cm.

Artefatti locali: `build/movement-stop/20261003_211916_51f55650/results.json`, log e telemetria nella stessa cartella. Un primo tentativo del nuovo harness ha incontrato la condivisione Windows del log aperto; il lettore è stato corretto per consentire la scrittura concorrente e il test è stato ripetuto. Non è stato un fallimento del movimento.

## Regressione con ritardo e perdita

```powershell
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory GAME_PATH -Media -Prediction -OneWayDelayMs 27 -DropEvery 1000 -MaxDatagramBytes 1200 -Port 27036
```

**PASS completo su entrambi**: movimento/prediction con collisioni, raccolta oggetto e consumo di una pozione, lanterne con olio indipendente, audio/particelle/musica/effetti, salvataggio/caricamento, cambio Entrance Hall → Old Archives, sequenza ordinata di morte e ripresa. I processi escono sul timer. Relay: 5766 ricevuti, 5761 inoltrati, 5 scartati, nessuno sopra soglia; massimo 1200 byte. Log conservati in `build/movement-stop/wan-regression`. Il ritardo del relay è nominale e non è una misura di Radmin.

## Limiti

Questa prova usa due processi locali, non i vostri due PC. Verifica l'arresto dopo input digitali avanti/laterale e la stabilità della replica; non è una certificazione di tutta la campagna, di ogni controller o di ogni condizione di rete. Rimangono validi i limiti della build VPN, incluso il precedente fallimento della prova combinata con 100 ms e perdita periodica del 5%, finché non venga verificato nuovamente.

Entrambi devono usare il nuovo pacchetto. Aggiornare anche l'host è necessario perché la correzione interessa il corpo fisico autorevole. Estrarre in una cartella nuova, avviare `AmnesiaCoop.exe` da lì e conservare profili e salvataggi.
