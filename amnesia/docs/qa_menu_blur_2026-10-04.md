# Menu online, uscite e immagine nitida — 4 ottobre 2026

## Correzioni e comportamento

Il client prima eseguiva il reset di Esci/Salva ed esci senza fermare la sessione:
il successivo aggiornamento di rete ricreava la mappa e lo rimetteva in partita.
Ora entrambi i comandi terminano volontariamente la sessione prima del reset.
Il salvataggio host viene catturato prima di cessare l'autorità; il client
continua a non salvare una replica incompleta.

Il protocollo **7** distingue l'uscita volontaria dal timeout tramite il codice
ENet `0x414f0001`, consegnato in modo affidabile e non bloccante. Il ruolo locale
diventa subito Offline; Poll completa la chiusura in background entro tre
secondi. Se esce il client, l'host aspetta sul proprio listener senza migrare.
Se esce l'host, il client torna al menu. Il recupero delle cadute resta separato.

**Riconnettiti** in Co-op online riprova ruolo, endpoint e identità precedenti.
Funziona dal menu dopo un'uscita e per riprovare una sessione ancora aperta.
Per un endpoint nuovo si usa Connetti. L'host che ha scaricato la mappa torna
alla lobby e deve avviare Continua salvataggio.

Il giocatore precisa che il blur riguarda entrambi ed è presente all'ingresso.
Lo script originale di Rainy Hall imposta intenzionalmente ImageTrail a 2;
questo conferma una fonte di sfocatura nell'introduzione, senza provare ogni
possibile causa grafica della segnalazione.

**Immagine nitida**, attiva di default e configurabile su ogni PC nella schermata
online, disattiva il rendering di ImageTrail e RadialBlur. Parametri, script,
fade e preferenze grafiche originali continuano a funzionare. Disattivando la
casella si rivedono gli effetti ai parametri correnti. È una preferenza esplicita
per ridurre il blur online, non una modifica agli asset o ai timer dell'intro.

## Build e prove

Release x64 MSVC v145; compilazione incrementale finale completata con zero
errori e zero avvisi. La precedente ricompilazione dei file dipendenti dagli
header aveva prodotto due avvisi C4244 di conversione wchar_t/char.
SHA-256 Lux.exe finale:
`A3D64CA69A63ED6FA3BAD87A24E61D3D343EAE9655ED03D838E372B30B84EEFD`.

**PASS** della suite di trasporto ASAN, inclusi 100.000 casi deterministici di
codec malformati, handshake/versioni, code, journal, checkpoint, riconnessione
e inversione dei ruoli. Nuova regressione: chiusura immediatamente Offline,
ricezione del motivo volontario da entrambi i ruoli, listener host conservato,
successiva riconnessione e cancellazione del motivo precedente.
Log in `build/online-menu-transport.log`.

**PASS** nativo con due processi e profili QA separati in Entrance Hall:

- Iniezione di ImageTrail 1,8 e RadialBlur 0,12 su ciascun processo. I parametri
  restano positivi, mentre i post-effect non vengono renderizzati.
- Il checkbox reale viene disattivato e riattivato tramite la propria callback:
  gli effetti tornano attivi, poi vengono nuovamente soppressi. Non viene
  modificato il flag grafico Disabled.
- Il client esegue Esci, resta Offline senza mappa al menu per dieci secondi,
  poi preme il pulsante reale Riconnettiti e riceve una baseline completa.
- Ripete con Salva ed esci, due secondi al menu e un'altra riconnessione. Host
  e client completano tre handshake; l'host non cede l'autorità.

Questi percorsi usano il reset effettivo del menu e i widget HPL originali;
non coprono manualmente tutte le popup o ogni combinazione di input/gamepad.
Artefatti locali in `build/online-menu-local`. Questa prova LAN usa il binario
precedente `E536A8C40BED0812FC4363894D99C3E8925CE6B34E0222174C04E2CDCC898D72`;
l'ultima modifica conserva anche l'endpoint effettivo dopo un cambio di autorità.

La stessa prova **passa sul binario finale con 100 ms nominali e perdita
deterministica del 5%**:
tre handshake, due uscite volontarie e due riconnessioni, checkbox e baseline
complete. L'host riceve due disconnect con motivo `1095696385` e non migra.
Il relay riceve 4.873 datagrammi, ne inoltra 4.630 e ne scarta 243; dimensione
massima 1.200 byte, nessuno scarto per dimensione. Artefatti:
`build/online-menu-final-wan100-loss5`. La prova precedente resta conservata in
`build/online-menu-wan100-loss5`.

**PASS sul binario finale** anche per una vera interruzione del trasporto e
migrazione tra due processi: il client promuove il checkpoint completo,
inverte gli stati individuali e ripristina la variabile di campagna e il journal
ordinato; il vecchio host si ricollega come client. Risultati di worker ormai
obsoleti vengono scartati e un successivo checkpoint valido viene accettato.
Con ritardo di validazione iniettato, il lavoro principale client registrato
resta sotto 1 ms; la cattura/serializzazione host registrata resta sotto 13 ms.
Artefatti in `build/recovery-qa/migrate`.

**PASS della regressione di campagna** con 70 ms nominali e perdita ogni 1.000
datagrammi sul binario precedente E536: movimento con prediction e collisioni,
raccolta/inventario, due lanterne, modello in prima persona, audio/particelle,
salvataggio/caricamento, checkpoint, passaggio Entrance Hall → Old Archives
e morte/ripristino autorevoli. Il relay riceve 7.192 datagrammi, ne inoltra
7.185 e ne scarta 7, tutti entro 1.200 byte. Le finestre ordinarie degli stati
client registrano medie di 16–17 ms e massimi di 33–50 ms. Una finestra del
cambio livello registra invece media 32 ms e massimo **1.832 ms**: il log host
riporta contestualmente **1.842 ms senza servizio del trasporto** durante
caricamento e autosalvataggio di Old Archives. Il caricamento client sospende
il trasporto per altri 778 ms. Non è una prova di assenza di pause: il caricamento
sincrono del motore resta un limite, mentre la prova completa senza blocco
permanente delle code o perdita della sessione. Artefatti:
`build/online-menu-campaign70`.

## Limiti

La verifica del blur riguarda i post-effect registrati nel motore, non una
comparazione manuale di screenshot sui PC remoti. Bloom, distorsioni di sanità,
fog, risoluzione e altre impostazioni grafiche conservano il comportamento
originale. La campagna intera e Radmin sui due PC dell'utente non sono certificati.
Entrambi devono aggiornare alla stessa build del protocollo 7.
