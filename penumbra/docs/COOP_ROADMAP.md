# COOP_ROADMAP — Penumbra: Overture co-op

Obiettivo: tutta la campagna di Overture giocabile in co-op da 2 a 4 giocatori, senza rubber banding evidente né desincronizzazioni bloccanti. Black Plague e Requiem solo dopo il completamento di Overture.

## Ciclo di lavoro

1. Scegliere il problema aperto più importante.
2. Trovarne la causa nel codice, con REA e Ghidra solo per codice senza sorgente.
3. Correggere.
4. Compilare.
5. Eseguire i test di protocollo e le prove native pertinenti.
6. Correggere le regressioni.
7. Eseguire la serie completa.
8. Documentare il risultato (`COOP_TEST_RESULTS.md`, `COOP_STATUS.md`).
9. Fare un commit locale.
10. Passare al problema successivo.

Niente push senza autorizzazione esplicita.

## Priorità

1. **Stabilità** (Fase 2, in corso):
   - ✅ coda di input;
   - ✅ previsione con le regole native;
   - ✅ coerenza delle collisioni, prima parte: replay con il corpo nativo del motore (§8 del rapporto);
   - ✅ collisioni fra personaggi come nell'originale (i giocatori si attraversano, urtano i nemici) e collider dei nemici sul client;
   - ✅ recupero degli arretrati dopo blocchi lunghi;
   - ✅ buffer di interpolazione temporale (protocollo 5);
   - poi previsione durante il trascinamento.
2. ✅ **Prova reale su due PC** via Radmin, raccogliendo `[COOP/NETDIAG]` da entrambi (prove automatiche; resta una partita a mano).
3. **Inventari separati e oggetti unici tracciati**, prima di passare a 4 giocatori: cambiano salvataggi e protocollo.
4. **Da 2 a 4 giocatori.**
5. **Modello e animazioni del partner.**
6. **Menu nel gioco, poi Steam** (dopo lo studio dei requisiti Steamworks).
7. **Black Plague, poi Requiem**, con uno strato di adattamento per ogni gioco sopra la rete comune.

## Fasi

| Fase | Contenuto | Stato |
| --- | --- | --- |
| 1 | Analisi del progetto esistente | ✅ |
| 2 | Stabilità: movimento, fisica, autorità, interazioni | in corso |
| 3 | Campagna completa di Overture, inventari, consumabili, oggetti unici, spettatore | in corso: inventari per giocatore fatti |
| 4 | Da 2 a 4 giocatori | in corso: rete, movimento, silhouette, nemici, checkpoint, scelta nel launcher e inventari per giocatore fatti |
| 5 | Presentazione e accesso: modelli, animazioni, audio, menu nel gioco, Steam | da fare |
| 6 | Validazione completa: automatica per ogni livello e reale su due PC | da fare |
| 7 | Black Plague, poi Requiem | non iniziato (su richiesta) |

## Compito corrente

Fase 4 (da 2 a 4 giocatori): fatti i passi 1-7 di `COOP_ARCHITECTURE.md`. Restano da provare la rete degradata con 3-4 giocatori e una partita reale su più PC.

Fase 3: fatti gli inventari per giocatore (passi 1-5 di `COOP_ARCHITECTURE.md`, inventario per giocatore).

Prossimo:
- partita giocata a mano su due PC;
- video affiancati di host e client durante le prove su due PC.

### Rimandato, con motivo

- **Personaggi animati di QuasiInteractive:**
  - i modelli non hanno una licenza dichiarata;
  - le modifiche del caricatore COLLADA cambiano il calcolo delle rotazioni per tutte le animazioni. Se integrate, vanno limitate ai file senza blocco `<extra>` di HPL, così gli asset originali restano identici.

  Dettagli in `QUASI_INTEGRATION.md`.

## Fatto di recente

- Bug del primo test reale corretti:
  - uscita con la X della finestra;
  - schermo intero e risoluzioni reali del monitor;
  - descrizioni e pensieri: messaggi personali, clic del client non girato all'host;
  - cigolii infiniti delle porte;
  - suoni doppi sul client.
- Spettatore per chi è a terra; log con orario, sessione e rotazione (`COOP_LOGS.md`); FPS e suoni nel log.
- Schede Intel HD: corretto lo schermo bianco (bloom) e la sovraesposizione della qualità "Alta", causati da letture "ombra" involontarie negli shader.
- Prime prove su due PC reali via Radmin VPN: `full`, `hammer`, `campaign` e `resume` PASS su entrambi i ruoli.
- Diario personale: le note sono di tutti, ma si aprono a chi le raccoglie; ognuno ha le sue note da leggere, anche nei salvataggi (protocollo 8).
- Passare un oggetto a un compagno con il gesto del gioco: oggetto scelto, mira, clic. Una unità delle pile; gli oggetti di gruppo restano a tutti.
- Inventari per giocatore:
  - un inventario per ospite sull'host, scambiato da `PlayerScope`;
  - oggetti di gruppo dati a tutti, consumabili e oggetti della storia a chi li raccoglie;
  - callback degli script in comune; `HasItem` e `RemoveItem` cercano in tutto il gruppo;
  - inventari degli ospiti nei salvataggi.

  Corretto un crash dell'host al ricaricamento del checkpoint, dovuto all'accumulo degli inventari degli ospiti nel salvataggio. Tutte le prove native passano.
- Da 2 a 4 giocatori, passi 1-4:
  - protocollo 7 con gli ID dei giocatori;
  - `HostTransport` con una `Session` per ospite;
  - stato per ospite sull'host: epoch, simulazione, eventi, silhouette;
  - altri ospiti visibili sul client;
  - ospiti piazzati in punti liberi.

  La prova `party` passa con 3 e 4 istanze.
- Da 2 a 4 giocatori, passi 5-6: nemici, attacchi ad area e checkpoint per tutti i giocatori, ingresso in corsa degli ospiti 3-4. Prova `party` estesa con nemico e checkpoint.
- Ospite 3-4 disconnesso: corpo tolto dal mondo, rientro nella stessa posizione e con lo stesso stato. Corretto un crash: un nemico poteva tenere come bersaglio un giocatore uscito dal mondo.
- Protocollo 6: oggetti tenuti, porte, oggetti leggeri, teletrasporti, correzioni smorzate fino a 1 m.
- Prova hammer resa significativa e passante da 0 a 120 ms di RTT.
