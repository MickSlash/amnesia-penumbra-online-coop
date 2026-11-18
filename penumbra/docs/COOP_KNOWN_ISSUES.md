# COOP_KNOWN_ISSUES — difetti aperti

Ogni voce indica se è misurata, verificata nel codice o solo ipotizzata. Le voci risolte passano in `COOP_TEST_RESULTS.md` con la misura che lo dimostra.

## Overture

- **Spingere oggetti leggeri camminando.** Sull'host il personaggio del client spinge gli oggetti dinamici, sul client gli oggetti replicati sono statici: il client si ferma e viene corretto finché l'oggetto replicato non si sposta.
- **Trascinamento in ritardo.** Mentre il client trascina oggetti, porte o leve, segue l'host con un RTT di ritardo.
- **Neve dell'esterno visibile solo sull'host.**
- **La prova `hammer` teletrasporta il giocatore a 39,5 s:** produce una correzione da 157 cm che è un artefatto del test.


- **La prova `hammer` fallisce il controllo di fluidità** (`max_horizontal_step_m < 0.2`), perché misura insieme rete, teletrasporti dello script e urti con i frammenti delle casse. Da separare: compito della roadmap.
- **Mai provato su due PC reali.** Tutte le misure vengono da due processi sullo stesso PC con relay simulato.
- **Inventario condiviso**, invece di inventari per giocatore.
- **Solo 2 giocatori.**
- **Partner rappresentato da una sagoma di box**, senza modello né animazioni.
