# Old Archives: prova nativa di rete e visuali — 4 ottobre 2026

## Esito e ambito

Avviati realmente due processi Lux.exe, con finestre visibili e profili QA
separati, sulla seconda mappa della campagna dopo Rainy Hall:
`01_old_archives.map`. Sono stati ispezionati gli schermi host e client e
conservati screenshot. Il client è stato mosso attraverso i normali percorsi
Move/SendLocalInput del motore, senza teleport, callback script invocate
direttamente o fixture aggiunte al mondo.

Le due sessioni completano handshake, baseline e checkpoint senza riconnessioni,
crash, checkpoint rifiutati o code che rimangono ferme. Non vengono riprodotti
i precedenti intervalli di centinaia/migliaia di millisecondi durante il gioco.
Questo esito riguarda le sezioni provate e le misure del trasporto; non certifica
la fluidità di ogni frame, la campagna completa o Radmin sui due PC remoti.

La mappa viene avviata direttamente con un profilo nuovo: non eredita inventario
e stato narrativo di una partita completata in Rainy Hall. Il percorso copre
ingresso, corridoio iniziale e stanza laterale, non i puzzle o l'uscita del livello.
L'host rimane fermo; si verifica il movimento autorevole del secondo giocatore
e la replica verso il client. Non è una prova del movimento simultaneo di due
giocatori lungo tutta la mappa.

## Binario e risultati

Release x64, protocollo 7, stesso eseguibile distribuito nello ZIP
`AmnesiaOnlineCoop_20261004_100010_688156.zip`.
SHA-256: `A3D64CA69A63ED6FA3BAD87A24E61D3D343EAE9655ED03D838E372B30B84EEFD`.
Non sono state apportate modifiche al binario durante queste prove.

| Misura | Ritardo aggiunto 70 ms, perdita 1/1.000 | Ritardo aggiunto 100 ms, perdita 5% |
| --- | ---: | ---: |
| Durata configurata del gioco | 180 s | 210 s |
| Intervalli degli stati client registrati | 9.784 | 10.996 |
| Medie delle finestre di cinque secondi | 17 ms | 17–19 ms |
| Massimo intervallo registrato | 50 ms | 67 ms |
| Checkpoint completi ricevuti dal client | 9 | 10 |
| Handshake per ciascun ruolo | 1 | 1 |
| Datagrammi ricevuti/inoltrati/scartati | 22.161 / 22.139 / 22 | 24.373 / 23.155 / 1.218 |
| Datagramma massimo | 1.199 byte | 1.200 byte |
| Datagrammi scartati per dimensione | 0 | 0 |
| Checkpoint host, massimo lavoro sul thread principale | 15,701 ms | 19,580 ms |
| Checkpoint client, massimo lavoro sul thread principale | 0,622 ms | 0,603 ms |
| Replica client, massimo dopo la prima finestra | 22,519 ms | 28,344 ms |

Il relay aggiunge 35 o 50 ms **per direzione**, con perdita deterministica.
Questi numeri sono ritardi artificiali aggiunti, non RTT misurati di 70/100 ms:
il log client registra RTT ENet di 122–175 ms e 150–207 ms rispettivamente,
includendo avvio e scheduling dei processi. La prova resta locale, senza Radmin.

Il client percorre circa 24,94 e 58,84 metri orizzontali, calcolati da campioni
di posizione ogni 250 ms: è una misura approssimata della traiettoria, non una
misura della fluidità frame per frame. Il secondo percorso raggiunge cinque
waypoint; altri due incontrano ostacoli. La guida automatica viene corretta con
obiettivi intermedi mantenendo collisioni e movimento ordinari. Questi timeout
del percorso non sono trasformati in completamento del livello.

## Pause e osservazioni

Resta una pausa iniziale durante il caricamento della replica: **1.155 ms** nella
prima prova e **1.172 ms** nella seconda. I log la collocano prima del primo
snapshot autorevole. Non sono registrate ulteriori pause del servizio del
trasporto sopra 250 ms durante le sezioni di gioco provate.

I picchi brevi riportati nella tabella restano reali: serializzazione/cattura
host fino a circa 20 ms e replica client fino a circa 28 ms possono incidere
sul tempo di un frame. Non sono stati nascosti né usati per promettere zero
micro-scatti. Il massimo di replica includendo la prima finestra supera un
secondo perché include il caricamento della mappa.

Si vedono torce accese all'ingresso, poi buio, polvere e hint durante gli eventi
originali. Lo script di Old Archives spegne lampade e cambia FOV/effetti/audio:
questo spiega quei cambi osservati, senza certificare ogni effetto o ogni evento.
Non viene verificata qui la raccolta della lanterna originale.

Sono presenti gli avvisi di cache MAP_CACHE e sampler già osservati nelle altre
prove riuscite, oltre al riferimento originale a un'entità mancante sul server.
Non compare un rifiuto applicativo di connessione o salvataggio. Le disconnessioni
conclusive osservate dall'host sono immediatamente seguite dall'uscita temporizzata
QA; non costituiscono una caduta durante la partita.

## Riproduzione e artefatti

Driver aggiunto: `tests/network/RunLevelNetworkProbe.ps1`. Le ultime modifiche
del driver aggiungono l'esportazione automatica delle finestre di misura; il
binario e il comportamento di gioco restano quelli effettivamente provati.

```powershell
./tests/network/RunLevelNetworkProbe.ps1 -GameDirectory GAME_PATH -Port 27080 -OneWayDelayMs 35 -DropEvery 1000 -Seconds 180 -Visual
./tests/network/RunLevelNetworkProbe.ps1 -GameDirectory GAME_PATH -Port 27082 -OneWayDelayMs 50 -DropEvery 20 -Seconds 210 -Visual
```

- `build/level-network-probe/20261004_110239_7118001c`: prima prova riuscita,
  movimento guidato manualmente attraverso il file QA, log e `measurements.json`.
  Screenshot `host-opening.png` e `client-script-event.png`.
- `build/level-network-probe/20261004_110604_aaad419b`: seconda prova riuscita,
  percorso automatico con obiettivi intermedi manuali, log e `measurements.json`.
  Screenshot `host-script-event.png`, `client-room.png`, `client-obstacle.png`.
- `build/level-network-probe/20261004_110220_ace515d6`: tentativo iniziale del
  driver interrotto per un nome PowerShell riservato; escluso dalle misure.
  Nella prima prova riuscita il driver non riconosceva il prefisso `maps/main/`
  della telemetria: è stato corretto, e il movimento in quella sessione è stato
  impartito manualmente tramite lo stesso file di input QA.

Le catture sono screenshot statici ottenuti da computer-use; non sono un video
continuo o un benchmark indipendente della presentazione dei frame. Entrambe
le istanze di gioco e i relay QA sono chiusi al termine. Installazione, profili
e salvataggi dell'utente non vengono modificati.
