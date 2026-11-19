# QA 2026-11-19: salto, inclinazione Q/E e raccolta degli oggetti (0.6.1)

Segnalazione dopo una partita reale con la 0.6.0 (video del client): rubber
banding risolto in gran parte; restano un salto che sul client sembra doppio e
in ritardo, Q/E a scatti e un piccolo ritardo nel raccogliere olio, acciarini e
note. Prove su un solo PC, attraverso il relay con 50 ms per direzione e il 5%
di perdita.

## Salto

Causa: il client stimava il salto con una formula separata
(`VerticalPrediction`). Alla conferma dell'host passava all'altezza dell'host,
indietro di un RTT; in più l'host segnala «a terra» per circa 10 tick dopo lo
stacco (contatore `mlOnGroundCount` di HPL). La visuale saliva, ricadeva e
risaliva.

Correzione: anche l'asse verticale è rieseguito con il corpo del motore
(`NativeStepper`): spinta del salto al fronte del tasto, gravità ridotta per
`MaxJumpCount` come in `cLuxMoveState_Normal`, gradini e cadute. Un salto già
in corso sull'host si ricava dalla gravità ridotta nello snapshot, non dallo
stato «a terra». Il recupero di due comandi in un tick non perde più il fronte
del salto.

Prova `-Effects` con una traccia tick per tick e un controllo nuovo («la
visuale risale dopo essere scesa di oltre 3 cm prima di atterrare»):

| Modello | Altezza massima client | Host | Doppio salto |
| --- | --- | --- | --- |
| Precedente (`AMNESIA_NET_PREDICTION=kinematic`) | 1,037 m (sale a 0,86, ricade a 0,81, risale) | 1,097 m | sì |
| Nuovo | 1,077 m, arco unico | 1,097 m | no |

## Inclinazione Q/E

Causa: l'inclinazione la calcolava solo l'host; il client applicava rotazione
e spostamento della testa del pacchetto più recente, quindi con un RTT di
ritardo e a scatti di pacchetto.

Correzione (protocollo 18): lo snapshot porta l'inclinazione separata
(`leanMove`, `leanRoll`); il client calcola la propria con `cLuxPlayerLean`
(collisioni della testa comprese) e la sostituisce a quella dell'host, tolta
lungo la direzione dell'host. Anche la rotazione del compagno è interpolata.

Prova `-Lean` attraverso il relay: la visuale del client si inclina dal tick
successivo alla pressione (prima rotazione a 43-53 ms); quella dell'host arriva
dopo circa 240 ms.

## Raccolta degli oggetti da inventario

- Il clic si lega al comando già spedito nel tick (prima +1: un tick di attesa
  in più sull'host).
- L'host non conta più come presente un'entità già destinata alla
  distruzione, e dopo un'interazione del client replica il mondo al tick
  seguente invece che al prossimo turno a 20 Hz.
- Il client nasconde subito l'oggetto e suona il suono di raccolta; se l'host
  non lo raccoglie entro 1,5 s l'oggetto riappare. Prova: un oggetto
  «raccolto» dal client senza conferma dell'host sparisce e torna.

## Fonti

Parte delle cause (contatore «a terra», recupero dei comandi, +1 del clic,
entità distrutte, cadenza a 20 Hz, yaw dell'inclinazione) viene da un'analisi
esterna, verificata sul codice prima di ogni modifica.

## Serie completa

**Correzione (20 novembre):** qui c'era scritto «18 prove di gioco su 18». Erano
16 su 18: le due prove delle leve (`-Levers`, anche con jitter) erano fallite,
ma il conteggio le dava per superate perché la riga d'errore contiene il testo
«PASS client pulled the lever» (la condizione mancante). 22 prove del
protocollo, invito Steam e rientro con Steam finto superati. Nessuna prova su
due PC con la 0.6.1. Causa del fallimento e correzione nella sezione 0.6.2.

## 0.6.2: suoni del giocatore sul client

Segnalazione: sul client mancano i passi e il suono dell'acciarino (la
raccolta degli oggetti si sente), notati dopo il caricamento della mappa
successiva.

Causa: passi, acciarino, olio, pozioni, voce del salto, battito e respiro
della sanità sono suoni GUI (`cLuxHelpFuncs::PlayGuiSoundData`), suonati sul
PC che simula il giocatore: per il client sull'host, come suoni dell'host. La
mod inoltrava solo la lanterna e i suoni degli script.

Correzione: `cLuxSoundOwnerScope` indica di quale giocatore sono i suoni
(aggiornamento del giocatore, azioni del client sull'host, passi e
atterraggio). `cLuxNetwork::RoutePlayerSound`, chiamata da
`PlayGuiSoundData`, manda il suono del giocatore del client al client (evento
`GuiSound` per lui, spedito a ogni tick) e, se è un suono «nel mondo», lo
suona sull'host nella posizione del compagno; i suoni nel mondo del giocatore
dell'host arrivano al client nella posizione dell'host. I suoni degli script
restano per entrambi; la raccolta prevista non si sente due volte. Nessun
cambio di protocollo.

Prove:
- `-Replication`: il client riceve 39 dei propri passi, l'host li suona nel
  mondo (39).
- `-Campaign` con due cambi di mappa e un conteggio nuovo dei suoni del mondo
  ogni 10 s: dopo il passaggio a `01_old_archives` host e client hanno gli
  stessi suoni (16 presenti, 13 in riproduzione); i passi del client arrivano
  anche dopo i cambi (76). Il cambio di mappa non perde suoni del mondo:
  mancavano sempre i suoni personali.

Porte verso un'altra mappa: il suono d'apertura (`ChangeMap`) e quello di
chiusura all'arrivo suonavano solo sull'host. Ora l'host suona l'apertura
per sé (chiunque abbia aperto la porta) e la manda al client; il suono d'arrivo
viaggia prima del cambio di mappa e il client lo suona quando ha caricato la
nuova mappa. Prova `-LevelDoor` (porta verso `09_back_hall` con i suoni di una
porta di legno originale): «Arrival sound played: door_level_wood_close».

Durante le prove è emerso un mio errore, corretto prima della pubblicazione:
gli eventi `GuiSound` non accettano valori negativi e la posizione dei passi
dell'host ne aveva; la coda si sarebbe bloccata appena l'host camminava. La
posizione ora viaggia come testo, ogni evento è controllato prima di entrare
in coda, e la prova `-Replication` fa camminare anche l'host e richiede che
ciascuno senta i passi dell'altro (client: 39 propri, 7 dell'host).

Leve nella 0.6.1 (regressione, corretta nella 0.6.2): con la previsione
verticale nuova, se l'host è a terra su qualcosa che il client non ha uguale
(nella prova la piattaforma creata solo sull'host; nel gioco un oggetto spostato
o un appoggio creato da uno script), il corpo del client cadeva a ogni
ricalcolo e il client si vedeva circa 10 cm più in basso (piedi a 29,904 m
invece di 30,01 m). Ora, se all'inizio del ricalcolo l'host è a terra e sotto i
piedi del client non c'è niente, il client tiene l'altezza dell'host fino a un
salto. Le prove `-Levers`, `-Effects` e `-Stairs` (relay) passano.
