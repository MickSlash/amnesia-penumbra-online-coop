# Audio della lanterna del client — 4 ottobre 2026

## Causa e correzione

L'host applicava il comando lanterna del giocatore remoto e riproduceva il suo
suono GUI sul proprio PC. Il client applicava correttamente lo stato autorevole
con `abUseEffects=false`, necessario per non ripetere i suoni a ogni snapshot o
caricamento, ma non riceveva un evento audio separato.

Il helper della lanterna ora instrada i suoni al proprietario:

- giocatore locale host e giocatori offline: riproduzione GUI locale originale;
- giocatore remoto sull'host: evento `GuiSound` affidabile, destinazione client;
- replica client: il helper non genera suoni dagli stati ricevuti; riproduce
  soltanto l'evento autorevole destinato a quel PC.

L'evento usa il file configurato per quel giocatore e il moltiplicatore di volume
originale. Copre accensione, spegnimento, esaurimento dell'olio e tentativo di
accensione con serbatoio vuoto. All'esaurimento dell'olio si evita inoltre il
secondo suono di spegnimento, che il codice originale emetteva dopo il feedback
di serbatoio vuoto. Lo spegnimento automatico supera i permessi degli stati
scriptati che bloccano i comandi manuali, evitando ripetizioni del feedback a
ogni frame. L'attivazione delle mani, lo stato della luce e i callback
della mappa continuano a essere applicati.

Il protocollo resta **10**: si riutilizza un evento già supportato. Aggiornare
comunque entrambi i PC con questo pacchetto, perché la correzione dell'invio
risiede sull'host. I suoni GUI descritti sono quelli della propria lanterna;
non viene aggiunto un suono spaziale della lanterna del compagno.

## Prova nativa

La prova `RunGameSmoke.ps1 -Media` è stata estesa per usare normali comandi
lanterna client e osservare i canali audio effettivi, controllandone ID, stato
di riproduzione e volume positivo dopo l'aggiornamento audio del motore.
Verifica inoltre che il caricamento del salvataggio e gli snapshot successivi
non aggiungano suoni, e che tre toggle producano esattamente tre eventi.

Dopo cambio livello e morte/ripresa viene preparato un serbatoio quasi vuoto:
il normale aggiornamento del giocatore consuma l'olio e spegne la lanterna.
Segue un normale tentativo client di accensione a olio zero. Il totale atteso
è di cinque eventi: due accensioni, uno spegnimento manuale, un esaurimento
e un tentativo a vuoto. Le risorse della campagna e i profili reali non sono
modificati; i fixture vivono nei due profili temporanei di QA.

La prima prova ha evidenziato due problemi del test: i canali GUI vengono
avviati dall'aggiornamento audio successivo, non dalla creazione dell'evento;
la ripresa dopo la morte può disattivare la lanterna del fixture. La verifica
ora attende il canale valido e prepara esplicitamente il fixture di esaurimento.
Le tracce iniziali sono conservate in `build/lantern-audio-20261004`.

## Risultati finali

Release x64 MSVC v145: compilazione finale riuscita senza errori o avvisi.
La ricompilazione precedente degli header aveva due avvisi C4244 di conversione
`wchar_t` → `char`. SHA-256 dell'eseguibile verificato:
`97749E361D12400B94877E96F03B710E62EC9C7B13A372A576265743784DB3A9`.

Passa la prova nativa:

```powershell
tests/network/RunImpairedNetworkTest.ps1 -GameDirectory '<installazione>' `
  -Media -Prediction -SnapshotBlackout -Port 27206 `
  -OneWayDelayMs 50 -DropEvery 20 -MaxDatagramBytes 1200
```

Due processi del gioco, 100 ms nominali di andata/ritorno, perdita deterministica
del 5% e datagrammi massimi di 1200 byte. Il relay registra 7.121 ricevuti,
6.765 inoltrati e 356 scartati, senza datagrammi fuori limite.

Il client registra **esattamente cinque canali audio in riproduzione con volume
positivo**, tutti destinati al proprietario remoto: due `ui_lantern_on` e tre
`ui_lantern_off`. Passano anche i controlli di assenza di click al caricamento
e negli snapshot, luce di entrambi i giocatori, permessi locali obsoleti,
lanterna in prima persona, inventario, pickup, media salvati, cambio da Entrance
Hall a Old Archives, morte/ripresa condivisa, prediction e recupero degli
snapshot durante un blackout simulato.

Profili temporanei: `OnlineCoopSmoke_20261004211728_1f6d9b00_host` e
`OnlineCoopSmoke_20261004211728_1f6d9b00_client`. Risultati, log di entrambi i
processi, relay e hash sono conservati in `build/lantern-audio-20261004`.
La suite standalone di trasporto ASAN non è stata ripetuta: codec, protocollo
e implementazione ENet non sono cambiati in questa correzione.

La verifica tra processi locali con rete simulata non certifica l'intera campagna
né la riproduzione sul dispositivo audio del PC dell'amico o il tunnel Radmin.
