# Installare e giocare ad Amnesia Online Co-op

Build sperimentale Windows x64, protocollo 4, due giocatori. La campagna completa
non è ancora certificata: consultare `qa_audio_2026-10-03.md` nei sorgenti per le
prove eseguite. Entrambi devono possedere Amnesia: The Dark Descent e usare gli
stessi dati del gioco e la stessa build della mod.

## Installazione dal pacchetto

1. Estrarre lo ZIP completo in una cartella propria, per esempio
   `C:\Giochi\AmnesiaOnlineCoop`. Conservare `bin`, `tools` e `LEGGIMI.md`.
2. Non sostituire l'eseguibile Steam e non copiare i dati del gioco nello ZIP.
   Il launcher legge i dati dalla propria installazione, indicata con
   `-GameDirectory`. La mod usa salvataggi separati.
3. Il pacchetto contiene `bin\Lux.exe` e il runtime x64 `OpenAL32.dll` già presente
   sulla macchina di compilazione, con il backend x64 `wrap_oal.dll` quando
   disponibile. `manifest.json` contiene gli hash della build;
   `source.zip` contiene i sorgenti corrispondenti e le licenze.
4. Aprire PowerShell nella cartella estratta. I comandi seguenti impostano
   l'esecuzione degli script per quel processo PowerShell soltanto. Non occorre
   Visual Studio per giocare con il pacchetto compilato.

Se Windows segnala `MSVCP140.dll`, `VCRUNTIME140.dll` o `VCRUNTIME140_1.dll`
mancante, installare il [Microsoft Visual C++ Redistributable x64 ufficiale](https://aka.ms/vc14/vc_redist.x64.exe).
La build richiede anche un driver grafico con supporto OpenGL; gli altri runtime
importati sono componenti di Windows. OpenAL deve essere x64, anche se i dati
provengono dall'installazione originale a 32 bit.

## Avvio dell'host

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./tools/StartOnlineCoop.ps1 -Role Host -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' -Port 27020
```

L'host resta nel menu mentre l'altro giocatore si connette. Poi sceglie Nuova
partita o Continua. Non selezionare la modalità co-op locale/split-screen:
la connessione online crea il secondo giocatore automaticamente.

## Avvio del client

Sostituire `IP_HOST` e il percorso del gioco con quelli corretti sul secondo PC:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
./tools/StartOnlineCoop.ps1 -Role Client -Address 'IP_HOST' -GameDirectory 'F:\SteamLibrary\steamapps\common\Amnesia The Dark Descent' -Port 27020
```

Il client aspetta nel menu e carica automaticamente il livello dell'host.
Entrambi giocano con tastiera e mouse, nella propria finestra. I controlli sono
quelli configurati nel gioco; la lanterna si attiva con il relativo comando.
Si possono aggiungere `-FullScreen -Width 1920 -Height 1080` ai due comandi.
La console sviluppatore offre `net_status` e `net_ping` per controllare la sessione.

## LAN, Hamachi e Radmin VPN

- **LAN reale:** usare l'indirizzo IPv4 locale dell'host, per esempio
  `192.168.1.20`, nel parametro `-Address` del client.
- **Radmin VPN:** entrambi entrano nella stessa rete Radmin. L'host crea una rete
  con nome e password; il client vi entra e usa l'IPv4 Radmin dell'host.
  [Guida ufficiale Radmin](https://www.radmin-vpn.com/help/index.html).
- **Hamachi:** entrambi entrano nella stessa rete virtuale; il client usa l'IPv4
  Hamachi dell'host. [Sito ufficiale Hamachi](https://vpn.net/).

La mod ascolta su tutte le interfacce IPv4 e comunica via UDP, quindi queste reti
virtuali sono compatibili in base al funzionamento del trasporto. **Non è stata
eseguita una partita su due PC attraverso Hamachi o Radmin.** La connessione
effettiva dipende dalla raggiungibilità del tunnel e dal firewall.

Consentire `bin\Lux.exe` nel firewall sul profilo di rete utilizzato dal tunnel,
con traffico UDP in entrata sulla porta scelta dell'host (default 27020).
Tenere il firewall attivo. Usare la stessa porta su entrambi. Non serve una
ricerca automatica dei server: il client inserisce direttamente l'IP.
Con una LAN virtuale raggiungibile normalmente non serve inoltrare la porta sul
router. Senza VPN, una connessione Internet diretta richiede che la porta UDP
dell'host sia raggiungibile; la mod non include relay o NAT traversal.

## Salvataggi e comportamento di gioco

L'host salva lo stato autorevole della campagna, compreso il secondo giocatore.
I file sono sotto `Documenti\Amnesia\AmnesiaOnlineCoop_Host`, nella sottocartella
del profilo. Il client non crea una campagna separata da combinare con quella
dell'host. Per riprendere, ricollegare entrambi dal menu e poi far scegliere
Continua all'host. Salvataggio/ricaricamento durante la sessione è stato provato;
la ripresa dopo riavvio di entrambi i PC richiede ancora una prova completa.

Inventario e progressione sono condivisi; attivazione e olio della lanterna sono
individuali nella configurazione della campagna originale. Il partner vede la
luce e la posa dell'avatar; l'avatar attuale non ha una lanterna dettagliata in
terza persona. I nemici sono simulati sull'host e possono scegliere entrambi i
giocatori. Ogni nemico insegue un bersaglio alla volta, può cambiarlo e controlla
separatamente la visibilità dei due. Nascondersi rompe la visuale, ma il mostro
può continuare a cercare nell'ultima posizione conosciuta.

## Limiti ancora aperti

- Campagna completa, puzzle, inseguimenti, finali e modalità Hard da provare su
  due PC. Il primo livello è stato attraversato con un driver di input su due
  processi locali in una build precedente; non equivale a certificare tutti i livelli.
- Cronologia del journal al caricamento/join incompleta sul client.
- Alcuni effetti speciali, creazione/trasformazione delle luci e tipi di entità
  fuori dal roster attuale richiedono ancora copertura.
- Audio e particelle condividono asset, stato e ciclo di vita, ma non il campione
  audio esatto né il seme casuale di ciascuna particella. I suoni diretti non
  gestiti dagli hook coperti richiedono audit.
- Nessuna ripresa automatica dopo disconnessione, migrazione host, lobby o
  prediction del movimento. La latenza di rete può rendere il movimento meno fluido.

Se non si collega: verificare IP del tunnel/LAN, porta, versione e dati identici,
poi il firewall per l'eseguibile della mod. Gli errori sono nel `hpl.log` del
profilo host/client sotto Documenti\Amnesia. Il pacchetto attuale è locale: il
repository GitHub indicato non viene aggiornato automaticamente.
