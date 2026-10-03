# Diagnosi iniziale della prova Radmin — 3 ottobre 2026

Segnalazione: connessione riuscita con circa 54 ms di ping, cadute ripetute, schermata iniziale bloccata o diversa tra i PC, apparente avanzamento premendo Esc. Il client remoto non è accessibile da questo workspace: il relativo log è ancora da ricevere.

## Evidenza nel log dell'host

Letto il log reale `Documenti/Amnesia/AmnesiaOnlineCoop_Host/hpl.log`, terminato alle 16:17:40 (Europe/Rome). Copia locale conservata in `build/vpn-freeze-qa/user-host-original.log`; non contiene log QA di una prova sintetica.

- 15 handshake accettati, 15 disconnessioni ENet e 15 annunci della stessa mappa `00_rainy_hall.map`.
- Nessun `Rejected`, `Client map readiness timed out` o rifiuto della fingerprint nel log di questo host.
- Ad ogni ciclo il server invia la baseline completa e cattura un checkpoint compresso di circa 99–100 KiB.
- L'invio della baseline richiede MapReady dal client: il trasporto ha superato l'handshake e il client ha dichiarato di aver caricato la mappa. Ciò non prova che abbia poi applicato tutta la baseline o mostrato l'introduzione.
- Il messaggio ENet corrente non distingue nel testo un timeout da una chiusura decisa dal peer. Un rifiuto rilevato solo sul client può quindi apparire sul server come una generica disconnessione.

Gli avvisi della cache MAP_CACHE e dei sampler grafici compaiono anche nelle prove locali riuscite. Non sono sufficienti, da soli, per attribuire loro le disconnessioni. Il log non riporta timestamp per ciascun evento: non è possibile ricavare un intervallo esatto tra le cadute.

## Riproduzione locale

Esteso `RunImpairedNetworkTest.ps1` con `StartMap` e `MapOnly`, mantenendo invariata la prova predefinita. I log sono ora raccolti dalla cartella corretta anche per le prove MapOnly.

Comando eseguito sulla build Steam già distribuita, senza modificare il binario:

```powershell
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory GAME_PATH -StartMap 00_rainy_hall.map -MapOnly -OneWayDelayMs 27 -DropEvery 1000 -Port 27044
```

Due istanze locali, 27 ms di ritardo per direzione, perdita deterministica di un datagramma ogni 1000. Relay: 8747 datagrammi ricevuti, 8739 inoltrati, 8 scartati, 3836319 byte inoltrati. Durante la finestra di circa un minuto il client ha caricato Rainy Hall, applicato baseline, journal, inventario e snapshot, e ricevuto tre checkpoint completi. Entrambi sono usciti sul timer QA. Il server osserva la disconnessione conclusiva del client prima della propria uscita; nessun ciclo di riconnessioni come nella prova reale.

Artefatti locali: `build/vpn-freeze-qa/repro-host.log`, `repro-client.log`, `repro-relay.log`.

Questo test non usa Radmin, non comprende due PC e non riproduce ogni passaggio manuale della lobby. Non dimostra che il problema reale sia risolto, né esclude jitter/perdita o differenze nei dati/installazioni dei due PC. Non sono state apportate correzioni speculative al gioco. Per proseguire serve soprattutto il log del client con i messaggi immediatamente precedenti alla prima caduta.
