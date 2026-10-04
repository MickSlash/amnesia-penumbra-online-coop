# Verifiche del prototipo — 5 ottobre 2026

Ambiente: Windows, MSVC v145 Win32, installazione Steam locale di Overture. Le prove native usano **due processi sullo stesso PC**, profili isolati e i dati originali in `redist`. Non è una certificazione della campagna o una prova su due PC remoti.

## Protocollo

`NetworkTests.exe`: **17 gruppi PASS**, inclusi 80.000 casi di input malformato e 20.000 ulteriori casi dei codec. Sono verificati limiti delle code, messaggi autorevoli, identificazione del giocatore, sequenze/epoch, caricamento bloccante di 6,5 secondi, scadenza del timeout di caricamento di 45 secondi, riconnessione, MTU ridotto e stato di prediction limitato. Per la prova di scadenza viene avanzato l'orologio di ENet; non si attende fisicamente per 45 secondi.

Questi 100.000 casi sono dati prodotti appositamente dal test, non pacchetti corrotti ricevuti durante una partita. Alcuni test riguardano strutture di journal, checkpoint e lobby del trasporto ereditato: non dimostrano che tali funzioni siano integrate in Overture.

## Gioco nativo

| Prova | Risultato verificato |
| --- | --- |
| Avvio del gioco ricompilato | Cabina iniziale caricata, uscita pulita |
| Collegamento host/client | Stessa mappa, baseline applicata, avatar sul client |
| Movimento del client | Spostamento di circa 1,4 m eseguito dal personaggio nativo sull'host |
| Presa della bottiglia | Stato Grab nativo; movimento fisico di circa 0,88 m replicato |
| Chiave del lucchetto | Richiesta del client eseguita dagli script sull'host; chiave rimossa e lucchetto disattivato su entrambi |
| Porta dell'armadietto | Stato Move nativo; trascinamento del mouse e transform del corpo articolato replicati |
| Cambio mappa | Cabina → `level01_01_outside.dae`, nuova epoch e barriera completata su entrambi |
| Introduzione normale | Sblocco dei comandi tramite messaggi e musica della campagna presenti su entrambi |
| Steam facoltativo | SteamAPI_Init/Shutdown; log Steam registra entrambi i processi come App ID 22180 |
| Launcher | Cartella principale/redist valide, controllo Steam/runtime e layout renderizzato |

Prova locale completa: `build/qa/coop-20261005-004138`. Prova finale completa con rete simulata e Steam: `build/qa/coop-20261005-005924`. Introduzione finale con Steam disattivato: `build/qa/coop-20261005-010050`. Il log Steam conferma il tracciamento di entrambi i processi del pacchetto sotto App ID 22180. L'eseguibile provato ha SHA256 `15C80305284A445C648D03E5C059F064A8C567D68B6EE5CA72EC6B9EB99EF827`.

Lo script `tools/Build.ps1` è stato eseguito con successo. CTest sulla build finale: 1 suite superata in 30,42 secondi, contenente i 17 gruppi del protocollo e i 100.000 casi descritti sopra.

Il test completo usa fixture, aziona input e richiede il cambio mappa: non risolve l'intera campagna mediante una partita manuale. L'introduzione viene provata separatamente, con gli script normali e avanzamento automatico dei messaggi. Le fixture ignorano l'input hardware reale per evitare che l'attività dell'utente modifichi la prova; mirano alla posizione corrente della bottiglia, che oscilla con la fisica della cabina.

## Rete con problemi simulati

Il relay introduce **35 ms per direzione** e scarta ogni ventesimo datagramma. Il ritardo aggiunto di andata/ritorno è 70 ms; il RTT misurato comprende anche elaborazione e scheduling delle due istanze. Non è una garanzia di RTT di 70 ms né una misura diretta di Radmin.

Nella prova finale: **7.940 datagrammi ricevuti, 7.543 inoltrati e 397 scartati intenzionalmente**; nessuno oltre il limite, dimensione massima 1.198 byte rispetto al tetto di 1.200. Il RTT misurato sul client verso la fine della prova è 124–127 ms. Presa della bottiglia: spostamento 0,881 m sull'host e 0,879 m sulla replica. Chiave, porta e passaggio alla mappa esterna superati su entrambi.

Il pacchetto è stato provato senza DLL originali vicino all'eseguibile della mod: vengono caricate dalla directory di lavoro `redist`. Gli screenshot esterni confermano cielo, nebbia e illuminazione ambientale simili; la neve a particelle resta presente soltanto sull'host. `validation.json` riporta le prove del binario distribuito; `manifest.json` identifica il commit dei sorgenti e gli hash dei file.

## Limiti delle verifiche

Nessuna prova completa di nemici, danni/morte del player 2, armi, nascondersi, notebook, salvataggio/ripresa co-op o migrazione host. Nessuna certificazione di assenza di lag su connessioni reali.

Nel log rimangono diagnostiche del codice originale: lo shader motion blur con ciclo dinamico usa il fallback statico previsto; l'asset `boat_dynamicparaffin.dae` segnala un joint/submesh mancante anche nella ricompilazione originale offline. I log delle prove riuscite terminano con `HPL Exit was successful` e memoria residua zero secondo il memory manager del motore; non è una verifica con AddressSanitizer.
