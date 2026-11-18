# COOP_KNOWN_ISSUES — difetti aperti

Ogni voce indica se è misurata, verificata nel codice o solo ipotizzata. Le voci risolte passano in `COOP_TEST_RESULTS.md` con la misura che lo dimostra.

## Overture

- **Spingere oggetti camminando:** ora i corpi entro la massa spingibile non bloccano il client (protocollo 6), ma restano statici sul client finché l'host non li sposta: si vedono muovere con un giro di ritardo. Non ancora misurato con una prova dedicata.
- **Porte, cassetti e leve trascinati dal client** rispondono dopo un giro in rete: 166 ms in locale, 350 ms con RTT 120 ms (`COOP_TEST_RESULTS.md` §10). La fisica gira solo sull'host. Possibile soluzione: possesso temporaneo della fisica dell'oggetto da parte del client, oppure corpi del client guidati a velocità come in QuasiInteractive (`QUASI_INTEGRATION.md`).
- **Colpi di martello del client con rete molto degradata** (RTT 120 ms + jitter 40 ms + 5 % perdita): in un'esecuzione su due, 4 colpi su 7 entro la finestra della prova. I gesti di attacco viaggiano sul canale affidabile e attendono le ritrasmissioni. Da misurare con più esecuzioni.
- **Neve dell'esterno visibile solo sull'host.**
- **La prova `hammer` teletrasporta il giocatore a 39,5 s:** produce una correzione da 157 cm che è un artefatto del test.


- **Mai provato su due PC reali.** Tutte le misure vengono da due processi sullo stesso PC con relay simulato.
- **Inventario condiviso**, invece di inventari per giocatore.
- **Solo 2 giocatori.**
- **Partner rappresentato da una sagoma di box**, senza modello né animazioni.
