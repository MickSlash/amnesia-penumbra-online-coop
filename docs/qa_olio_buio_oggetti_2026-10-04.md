# Olio iniziale, visuale al buio e movimento di porte/casse

Build Windows x64, protocollo 10. SHA-256 di `Lux.exe`:

`9AC031AF656E6DE5A17EEA3C75350E5EA7075112165BEA59AF2F4E5E1B0F93E8`

## Problemi individuati e correzioni

- `00_rainy_hall.hps` chiama `SetPlayerLampOil(0)` per Daniel. P2 partiva invece
  con il valore predefinito 100. L'adattatore online ora inizializza entrambi a
  100 dopo gli script iniziali, attraverso `OnGameStart`. Il valore non viene
  reinizializzato da caricamento, riconnessione o cambio mappa.
- Il client saltava `LuxPlayerLightLevel` e `LuxPlayerInDarkness` quando
  aggiornava i soli componenti visivi della replica. Ora aggiorna anche questi
  due helper sul giocatore locale; la perdita di sanità resta calcolata
  dall'host. Le luci `PlayerDarknessAmbient*` non sono replicate dalla posizione
  dell'host e non alterano i calcoli di luce/visibilità dell'altro giocatore.
- I prop trasferivano ogni posizione nello stesso canale affidabile del mondo
  e dei media, applicandola direttamente ai corpi del client. Ora le posizioni
  durante il movimento sono sostituibili, a 20 Hz, con frammenti non affidabili
  per gli oggetti articolati grandi. Il client conserva l'ultima posizione per
  ID e scarta stati vecchi fra i canali attraverso numeri di sequenza.
  Creazione, rimozione e proprietà sono affidabili; dopo 250 ms senza cambiamento
  viene inviata anche una posizione finale affidabile. I corpi vengono
  interpolati con rotazione quaternion su 50 ms; teletrasporti oltre 5 m scattano
  direttamente. L'interpolazione viene cancellata al cambio mappa.

## Prove

La suite `NetworkTests` compilata con AddressSanitizer passa: 100.000 casi di
codec malformati, handshake, code limitate, epoch, baseline, recupero MTU,
checkpoint, lobby, recupero e prediction. I nuovi casi coprono accorpamento
delle posizioni, scarto di posizioni affidabili precedenti, bypass della coda
bulk piena, rimozione senza resurrezione da pacchetto vecchio, prop con 32 corpi,
wrap della sequenza e cambio mappa.

La prova nativa finale su Entrance Hall passa con host e client reali,
profili QA separati, 35 ms imposti per tratta e perdita periodica del 5%.
Comprende una fixture che spegne le luci della mappa in memoria e muove una
cassa e una porta: non modifica gli asset installati. Entrambi verificano
olio 100 e luce di adattamento al buio senza lanterna. Il client verifica
matrici intermedie per entrambi gli oggetti e convergenza alla posizione finale
autorevole. I corpi di queste fixture sono mossi dal driver sul server: questa
prova non equivale a una partita manuale di trascinamento su due PC.

Contatori del relay: 6.970 datagrammi ricevuti, 6.622 inoltrati e 348 scartati;
dimensione massima 1.198 byte; nessuno scarto per dimensione. Un handshake per
giocatore. Intervalli degli stati del giocatore sul client: 2.914 campioni,
media ponderata circa 16,9 ms, massimo 49 ms. Gli intervalli misurano l'arrivo
degli stati; non sono il ping né il ritardo totale fra input e risposta.
Framerate medio del client circa 58,5 a 640×480, grafica ridotta.

Diagnostica del buio del client: livello locale 0,194764, canale rosso della
luce ambientale 0,119961, luce attiva, lanterna spenta e olio 100. La soglia
originale di adattamento è 0,2: la prima iterazione del test pretendeva
erroneamente un livello inferiore a 0,001. La correzione del criterio usa
ora la soglia della configurazione originale.

Artefatti: `build/visibility-props-20261004/asan-tests.log`,
`visibility-host-final.log`, `visibility-client-final.log`,
`visibility-relay-final.log` e `native-visibility-props-final.log`.

L'intro originale finale passa: quattro voci e sottotitoli su entrambi,
baseline e checkpoint, olio pieno, una sola connessione e movimento dopo
l'intro (host 1,776 m, client 2,931 m). Run
`build/campaign-start/20261004_201348_16890e81`; log riepilogativo
`native-intro-final.log`. La prima iterazione segnalava una caduta solo
durante la chiusura automatica del processo host: il timer QA ora chiude
volontariamente la sessione. La modifica è limitata alla modalità di test.

La regressione media/salvataggio finale passa con 50 ms per tratta e perdita
del 5%: lanterne con olio indipendente, luce e mani sul client, pickup,
inventario, audio, particelle, musica ed effetti, salvataggio/caricamento,
Entrance Hall → Old Archives, morte e ripresa condivise. Passa anche il
blackout di tre secondi degli snapshot ordinari, con gli stati di riserva.
Relay: 6.990 datagrammi, 6.641 inoltrati, 349 scartati, massimo 1.200 byte,
nessuno scarto per dimensione. Log `native-media-final.log`,
`media-host-final.log` e `media-client-final.log`. Le prove e l'hash sono
riepilogati in `build/visibility-props-20261004/verification.json`.

## Limiti

Queste prove locali non attribuiscono i problemi della rete reale a Radmin e
non certificano un tunnel fra due PC. L'interazione con oggetti fisici resta
autorevole sull'host e conserva il ritardo della rete. Perdite prolungate,
forte jitter e pause del computer possono ancora produrre correzioni del
movimento. La campagna intera e il trascinamento manuale su VPN devono ancora
essere verificati. Le nuove campagne ricevono olio pieno; i salvataggi già
esistenti conservano il proprio valore e la lanterna richiede ancora il pickup.
