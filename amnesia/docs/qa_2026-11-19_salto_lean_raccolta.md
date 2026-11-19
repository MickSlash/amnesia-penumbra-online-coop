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

18 prove di gioco su 18 superate (anche l'inclinazione attraverso il relay),
22 prove del protocollo, invito Steam e rientro con Steam finto. Nessuna prova
su due PC con la 0.6.1.
