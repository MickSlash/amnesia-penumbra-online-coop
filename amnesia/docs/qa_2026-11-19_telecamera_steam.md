# QA 2026-11-19: telecamera del client, inviti Steam, grafica del client

Prove su un solo PC (Windows 11, Amnesia Steam x64). Nessuna prova su due PC o
con il vero Steam tra due account: gli inviti reali vanno provati da due persone.

## Telecamera del client (rubber banding visivo)

Causa: la telecamera di HPL fa la media delle ultime 10 posizioni della testa
(`CameraSmoothPosNum = 10` in `game.cfg`) e resta indietro rispetto ai piedi
quando ci si muove. L'host inviava lo scostamento telecamera−piedi con dentro
quel ritardo; il client lo sommava ai propri piedi previsti con un RTT di
ritardo. Quando si partiva la visuale veniva tirata indietro, quando ci si
fermava scivolava in avanti. Le misure precedenti guardavano solo i piedi.

Correzione: l'host invia lo scostamento senza il ritardo
(`iCharacterBody::GetCameraSmoothLag`) e il client rifà la stessa media sui
piedi che mostra. `AMNESIA_NET_CAMERA=legacy` su entrambi i PC ripristina il
comportamento precedente per i confronti.

Misura nuova `[NET/CAMERA]`: distanza orizzontale tra la telecamera mostrata e
la media dei piedi mostrati, mentre ci si muove (prova con previsione, 11
finestre da 5 s per scenario). Il residuo di circa 2 cm è l'oscillazione della
testa.

| Rete (relay locale) | Modalità | Scarto medio | Massimo | Fotogrammi oltre 10 cm |
| --- | --- | --- | --- | --- |
| 50 ms, 5% perdita | precedente | 6,4–12,0 cm | 14,5–51,5 cm | 28–76% |
| 50 ms, 5% perdita | nuova | 0,7–2,0 cm | 3,0–6,0 cm | 0 |
| 40 ms, jitter 0–30 ms, 2% perdita | precedente | 6,6–12,7 cm | 16,2–55,1 cm | 32–67% |
| 40 ms, jitter 0–30 ms, 2% perdita | nuova | 1,2–2,2 cm | 2,9–5,5 cm | 0 |

## Altre correzioni della stessa versione

- Snapshot spediti nello stesso tick (`Session::Flush` dopo la replica), non
  al `Poll` del tick seguente: circa 16 ms in meno dall'host al client.
- Corsa: lo snapshot porta i rapporti della velocità massima con e senza corsa
  e lo stato di corsa; il client prevede subito Shift premuto o lasciato.
- Accovacciati: la previsione con il corpo del motore resta attiva (prima era
  spenta e il movimento da accovacciati andava a scatti).
- Scala a pioli e arrampicata: flag 128, niente previsione.
- Correzioni tra 25 cm e 1 m ammorbidite invece di scattare; oltre 1 m è un
  teletrasporto.
- Compagno interpolato sulla linea temporale dei tick dell'host invece che
  sull'orario di arrivo dei pacchetti.
- Flashback: rallenta anche il giocatore online (bug noto del README). Nuovo
  controllo nella prova `-Flashback`.
- Rotellina inoltrata all'host (distanza degli oggetti afferrati), azione 8.
- Cassetti: il pitch del client segue solo la sua impostazione del mouse
  invertito, non anche quella dell'host.
- Salto: con la coda dei comandi arriva solo dal bit del comando.
- Menu Co-op online in inglese; in italiano se il gioco è in italiano.

Serie completa prima degli inviti Steam: 17 prove di gioco su 17, 21 prove del
protocollo. Dopo gli inviti: 22 prove del protocollo (nuova: sessione, uscita e
rientro attraverso il ponte con un finto Steam in memoria); la serie di gioco è
riportata in fondo.

## Inviti Steam e rientro

`RunSteamInviteTest.ps1` avvia host e client con
`AMNESIA_NET_TEST_STEAM_FAKE` (Steam sostituito da porte UDP locali: due copie
dello stesso account Steam non possono invitarsi). Lobby, richiesta di ingresso,
ponte dei messaggi, sessione ENet e recupero sono il codice usato con Steam.

- Invito: il client entra solo dalla richiesta di Steam, riceve il mondo
  attraverso il ponte, entrambi escono puliti. PASS.
- `-Rejoin`: il client va in crash (processo terminato) a partita avviata;
  l'host mette in pausa; il client riavviato accetta di nuovo l'invito, l'host
  registra «Partner reconnected; rebuilding authoritative baseline» e il client
  riceve il mondo in corso. Nessun rifiuto d'identità né migrazione dell'host.
  PASS.

Non provato: il vero Steam (lobby, overlay, `ISteamNetworkingMessages` con i
relay) tra due account su due PC.

## Grafica «sgranata» lato client

Segnalazione: a un amico il client sembra sgranato.

- Nel codice non c'è nulla che riduca la qualità solo sul client.
- Confronto delle immagini reali delle due finestre (PrintWindow, stessa mappa,
  stesse impostazioni, 1280×720 in finestra): il client è pulito, senza grana.
  I punti «bianchi» nelle prime catture erano pixel con alfa 0 nel buffer
  (Windows li ignora a schermo), non pixel visibili.
- Causa probabile: il launcher creava il profilo a **1280×720 a schermo
  intero**. Su uno schermo 1080p o più grande l'immagine viene ingrandita e
  sembra sgranata. Chi ospitava aveva già scelto la risoluzione nativa
  (3440×1440), il compagno probabilmente no.
- Correzione nel launcher: a schermo intero usa `Width="0" Height="0"`
  (finestra senza bordi alla risoluzione del desktop, come le impostazioni SDL2
  originali del gioco). Un profilo rimasto al vecchio 1280×720 a schermo intero
  passa alla nativa; una risoluzione scelta nelle opzioni resta com'è.

## Serie completa dopo gli inviti Steam

17 prove di gioco su 17 superate: trasporto, diagnostica dei crash, campagna,
rete degradata (anche con cancello, scale, jitter ed effetti), giocatore
dell'host, arresto del movimento, recupero, avvio della campagna, nemico, leve
e ruote (anche con jitter), flashback (con il nuovo controllo sul rallentamento
del giocatore remoto) e inclinazione. 22 prove del protocollo superate.
