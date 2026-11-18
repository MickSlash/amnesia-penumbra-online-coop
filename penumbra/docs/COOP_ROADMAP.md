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
2. **Prova reale su due PC** via Radmin, raccogliendo `[COOP/NETDIAG]` da entrambi.
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
| 3 | Campagna completa di Overture, inventari, consumabili, oggetti unici, spettatore | da fare |
| 4 | Da 2 a 4 giocatori | da fare |
| 5 | Presentazione e accesso: modelli, animazioni, audio, menu nel gioco, Steam | da fare |
| 6 | Validazione completa: automatica per ogni livello e reale su due PC | da fare |
| 7 | Black Plague, poi Requiem | non iniziato (su richiesta) |

## Compito corrente

1. Ramo `quasi-integration`: integrazione delle parti utili di QuasiInteractive secondo il piano in `QUASI_INTEGRATION.md`.
2. Fase 4 (da 2 a 4 giocatori) secondo il progetto in `COOP_ARCHITECTURE.md`.

## Fatto di recente

- Protocollo 6: oggetti tenuti, porte, oggetti leggeri, teletrasporti, correzioni smorzate fino a 1 m.
- Prova hammer resa significativa e passante da 0 a 120 ms di RTT.
