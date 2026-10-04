# Avvio campagna online e intro del primo livello

Build Windows x64, protocollo **8**. SHA-256 dell'eseguibile verificato:
`F096FAAA3111E1746C0E8BCF5F88FE27F8B3905783DFE4F5808030F017EEC010`.

## Problema e diagnosi

Nel log del client remoto la prima mappa impiega 3499 ms e i precaricamenti
altri 466 ms. Il trasporto registra una pausa di 4053 ms sul thread del gioco.
Segue una perdita della connessione senza applicazione della baseline.
La connessione iniziale ha RTT di circa 43–51 ms e accetta il protocollo:
il solo ping non spiega il blocco. Non compaiono rifiuti per dati incompatibili.

Un confronto isolato con il trasporto della build precedente, ricostruito
dal commit c8ae73bc0c9e7229ae5a4fc768ab75e7634e4f75, conferma il difetto:
una pausa client di 6,5 secondi disconnette l'host e impedisce MapReady.
Prova locale in `build/campaign-start/legacy-timeout/result.log`:
`host_connected_after_6500ms=0`, `baseline_can_start=0`.
Non è una riproduzione identica del tunnel remoto o dei suoi tempi.

## Correzioni

- L'host invia e scarica sul socket un avviso prima del caricamento nativo.
  Entrambi usano una tolleranza ENet specifica del caricamento, con attesa
  minima di 30 secondi e massimo di 45. Il limite applicativo della ricostruzione
  del mondo è 30 secondi dall'annuncio della mappa. Dopo la conferma tornano
  i timeout ordinari: non è una modifica della frequenza degli snapshot.
- MapReady significa che il client ha caricato la mappa. La nuova conferma
  WorldApplied significa che ha applicato entità, media, presentazione iniziale,
  inventario, journal e primo stato del giocatore. I timer e la fisica dell'host
  aspettano questa seconda conferma; la replica continua durante l'attesa.
  La pausa per input assente si applica dopo questa fase.
- L'intro originale di `00_rainy_hall` condivide sul client la telecamera di
  Daniel: posizione, orientamento, roll, FOV e rapporto prospettico. Gli avatar
  vengono nascosti durante la sequenza e il client non può alterarla con
  movimento o interazioni. Al completamento del risveglio ritorna alla propria
  telecamera e al movimento indipendente. Voce, sottotitoli e fade usano il
  percorso di replica già presente.
- Protocollo incrementato a 8, con rifiuto delle build precedenti. Il formato
  dei salvataggi non cambia. I log aggiungono stato di caricamento e conferma
  della mappa/mondo, per distinguere le fasi in un'eventuale nuova segnalazione.

## Prove

`tests/network/RunCampaignStartTest.ps1` apre due istanze con profili nuovi,
entra dal pulsante nativo **Nuova campagna** della lobby e lascia eseguire
OnStart, timer, callback audio e risveglio originali. Non salta l'intro e non
richiama direttamente le sue funzioni. Introduce una pausa client di sei
secondi prima del caricamento, oltre al tempo effettivo del loader.
Relay: 35 ms per direzione, perdita periodica di un datagramma ogni 20,
limite di 1200 byte. L'RTT nominale di 70 ms non equivale a una misura sul VPN:
il ritardo misurato include programmazione e lavoro dei processi Windows.

La prova con il binario sopra, in
`build/campaign-start/20261004_153939_ca8000c1`, conserva una sola connessione
per ruolo, applica la baseline, produce/riceve checkpoint e termina pulita.
Su entrambi risultano avviati i quattro stream originali
`CH01L00_DanielsMind01_01` … `CH01L00_DanielsMind04_01`, con sottotitoli.
La verifica legge i canali nativi in riproduzione e il testo renderizzabile;
non è una registrazione o una valutazione umana dell'uscita audio fisica.
Il client usa la telecamera condivisa; dopo il risveglio i normali input
producono spostamenti di **2,142 m host** e **2,931 m client**.
Il relay registra 21.219 datagrammi ricevuti, 20.159 inoltrati e 1.060 scartati;
nessun datagramma supera 1200 byte. Il test termina con exit code 0.
Risultato completo: `build/campaign-start/final-intro-validation.log`.

La prima esecuzione del nuovo harness ha avuto due errori di asserzione:
nomi file audio attesi diversi dalle voci originali e marker checkpoint host
confuso con quello client. Sono corretti nell'harness. I log della partita
precedente sono stati rivalidati e conservati; la ripetizione completa sopra
supera tutte le asserzioni con l'harness corretto.

Altre regressioni sullo stesso binario:

- Suite trasporto con AddressSanitizer, 80.000 casi codec corrotti e ulteriori
  20.000 casi recovery: nessun errore ASAN. Pause di 6,5 secondi dell'host e
  del client, conferma del mondo, isolamento degli epoch, presentazione ancora
  in coda e bit della telecamera inclusi. Log: `build/network/loading-final-asan.log`.
- Prova nativa media/prediction: 100 ms nominali, 5% di perdita, 1200 byte;
  lanterne indipendenti, audio/particelle, stato di riserva durante blackout
  degli snapshot ordinari, salvataggio/caricamento, cambio livello e morte/ripresa.
  Log: `build/campaign-start/media-regression.log` e
  `build/campaign-start/regressions-F096/media`.
- Migrazione host, journal, worker checkpoint lento di 350 ms e scarto del
  lavoro diventato obsoleto: PASS. Log: `build/campaign-start/recovery-regression.log`
  e `build/campaign-start/regressions-F096/recovery`.
- Menu nativo con ritardo nominale di 70 ms e perdita del 5%: immagine nitida,
  Esci e Salva ed esci restano al menu, Riconnettiti ricostruisce la campagna.
  PASS su entrambi i ruoli. Log: `build/campaign-start/menu-regression.log`.

Screenshot conservati in
`build/campaign-start/20261004_153939_ca8000c1/client-intro.png`
(risveglio con telecamera inclinata, senza avatar davanti) e `host-intro.png`
(host dopo il risveglio). In
`build/campaign-start/20261004_153436_8a6fc1cd/client-after-intro.png` e
`host-after-intro.png` si vedono le visuali indipendenti dopo il movimento.

## Limiti

Le prove sono tra processi locali con dati Steam installati, non tra i due PC
della segnalazione e non attraverso Radmin. Il confronto con i dati DVD resta
da ripetere nella sessione reale. Gli screenshot sono osservazioni in momenti
successivi e non fotogrammi simultanei. L'intro originale contiene fade neri
e momenti di controllo limitato previsti dallo script.

Questa verifica copre l'avvio, l'intro e il movimento dopo il risveglio.
Non certifica il completamento del primo livello o dell'intera campagna,
né elimina i caricamenti sincroni del motore o ogni possibile micro-scattosità.
Aggiornare entrambi i PC usando il nuovo launcher nella cartella estratta;
conservare profili e salvataggi esistenti.
