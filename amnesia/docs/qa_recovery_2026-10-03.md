# QA menu, journal e recupero — 3 ottobre 2026

Build Windows x64, MSVC v145, protocollo 5. SHA256 Lux.exe della build finale:
`933D5D887C5D87ABF23628FEC12A954276D61145418AFA5A652388383427A79A`.
Dati Steam locali da `F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent`.
Le prove usano processi sullo stesso PC, con profili separati. Non sono prove su
due PC fisici, una VPN reale o l'intera campagna.

## Risultati

| Prova | Esito e osservazione |
| --- | --- |
| Release x64 | Compilazione completata |
| NetworkTests con AddressSanitizer | PASS: 80000 casi precedenti più 10000 mutazioni journal e 10000 checkpoint; nessuna diagnostica ASAN |
| Trasporto ENet | PASS: handshake, versione errata, input malformati, sequenze, timeout, map epoch, code limitate e baseline |
| Lobby e identità | PASS: entrambi Pronto, rifiuto di un'identità diversa, ripresa del partner e inversione host/client |
| Journal | PASS: una nota, due diari ordinati, un obiettivo attivo e uno completato trasferiti al join e dopo ripresa/migrazione |
| Riconnessione nativa | PASS: interruzione del trasporto per due secondi, riconnessione automatica e replica con nuova epoch |
| Migrazione nativa | PASS: checkpoint XML compresso, promozione del client, variabile campagna conservata, salute 99/73 invertita correttamente, ritorno del vecchio host |
| Validazione checkpoint nativo | PASS: classi sconosciute, vettori incompleti e factory dei contenitori incompatibili rifiutati prima della deserializzazione |
| Prediction | PASS: input pendente prima di ACK, limite 250 ms/32 comandi, ostacoli, soppressione durante interazioni; integrata nel gioco con rete degradata |
| Campagna con relay degradato | PASS: 50 ms per direzione, circa 5% perdite; 5410 datagrammi ricevuti, 5140 inoltrati, 270 persi, 1873877 byte inoltrati |
| Regressione con relay | PASS: pickup singolo, pozione, inventario, audio/particelle e ricaricamento, lanterne indipendenti, cambio Entrance Hall → Old Archives, morte/ripresa ordinata |
| GUI con mouse | PASS: launcher, profilo automatico, Co-op online, Ospita/Connetti, Pronto e Nuova campagna; entrambe le istanze caricano Rainy Hall |

Gli stress del codec sono input sintetici ai decodificatori: **non sono 100000
pacchetti UDP corrotti inviati al gioco**. La prova relay perde datagrammi reali,
senza modificarne i byte. Le prove native terminano normalmente e il memory
manager HPL riporta zero memoria residua; ASAN copre il test del protocollo,
non l'intero vecchio motore Newton/HPL.

Artefatti locali generati (ignorati da Git):

- `build/recovery-qa/transport-asan.log`
- `build/recovery-qa/reconnect/{host.log,client.log}`
- `build/recovery-qa/migrate/{host.log,client.log}`
- `build/recovery-qa/menu-{host,client}.log`
- `build/recovery-qa/screenshots/{launcher.png,host-lobby.jpg,client-lobby.jpg,host-campaign.jpg,client-campaign.jpg}`
- `build/impaired-network/{relay.log,host.log,client.log}`

Gli screenshot mostrano visuali locali diverse durante l'introduzione e i
sottotitoli della storia su entrambe. Sono istanti distinti, non una misura
frame-per-frame della sincronizzazione. Nelle automazioni alcuni clic rapidi
non erano acquisiti dal loop del gioco: la prova ha usato anche una pressione
più lunga del mouse. Il flusso reale è stato percorso nell'interfaccia, senza
variabili di rete, fixture o comandi console. Gli screenshot precedono l'ultima
stretta del validatore XML; il codice del menu è lo stesso.

## Correzioni emerse durante le prove

Il primo trasferimento non compresso con frammenti grandi sul canale del mondo
ritardava gli eventi affidabili sotto perdita. Ora il checkpoint è compresso,
trasferito sul proprio canale e limitato a 1 KiB per tick. La regressione con
relay passa. La lease degli input bloccava una nuova mappa prima della baseline:
ora viene riavviata dopo il primo input della nuova epoch.

Una prova preliminare di migrazione ha rifiutato il checkpoint nativo e non ha
completato il recupero. Dopo aggiunta delle diagnostiche e controlli più precisi,
le successive prove di migrazione sono passate, incluse due esecuzioni sulla
build finale. Il precedente rifiuto non è stato riprodotto e la sua causa non è
stata identificata con certezza: questo impedisce di certificare la migrazione
su ogni stato della campagna. Un checkpoint rifiutato non viene deserializzato
né confermato; senza un precedente checkpoint valido la campagna resta in pausa.
Le righe `Save schema rejected` nei test negativi finali sono intenzionali e
precedono il marker che conferma il rifiuto delle mutazioni.

## Limiti della validazione

La migrazione recupera l'ultimo checkpoint completo, con possibile perdita dei
progressi successivi. Richiede raggiungibilità bidirezionale e identità di ripresa;
non è consenso distribuito certificato per partizioni WAN. Non sono provati
spegnimento del PC, crash seguito da riavvio, tutti i salvataggi della campagna,
Hard, finali, tutti i puzzle e tipi di entità/script. Il menu Continua usa il
caricamento nativo dell'host, ma la riapertura manuale di entrambi i giochi da
salvataggio deve ancora essere verificata end-to-end.

Il log motore può contenere warning/ERROR preesistenti di cache della mappa,
sampler aGoboMap e StartPos vuoto durante ricaricamento della replica. Il test
controlla applicazione della posizione canonica e marker gameplay, non dichiara
che il motore originale sia privo di ogni warning. La prediction resta orizzontale;
verticale, crouch, salti e oggetti possono mostrare correzioni sotto latenza.

Per ripetere:

```powershell
./tests/network/RunTransportTests.ps1 -Executable ./build/network-asan/NetworkTests.exe
./tests/network/RunRecoveryGameTest.ps1 -GameDirectory 'GAME_PATH' -Mode reconnect
./tests/network/RunRecoveryGameTest.ps1 -GameDirectory 'GAME_PATH' -Mode migrate
./tests/network/RunImpairedNetworkTest.ps1 -GameDirectory 'GAME_PATH' -Media -Prediction
```

La [guida di installazione](online_installazione_it.md) descrive il percorso per
chi gioca, senza terminale, e i requisiti LAN/Hamachi/Radmin.
