# Compilazione verificata su Windows

Richiesti Visual Studio con strumenti C++ e Windows SDK, CMake, Git e .NET Framework. Configurazione verificata: Visual Studio 2026 Community, MSVC v145, **Win32**. Il runtime Newton originale è a 32 bit; non configurare x64.

Dal repository:

```powershell
./tools/Build.ps1
```

Lo script recupera soltanto header e import library SDK dalla revisione fissata del repository delle dipendenze; il gioco, il motore, AngelScript ed ENet sono già presenti nei sorgenti. I binari sono in `build/bin/Release/PenumbraCoop.exe` e `build/PenumbraCoopLauncher.exe`.

Per verificare i protocolli:

```powershell
ctest --test-dir build/windows -C Release --output-on-failure
```

Per le prove native, con i dati originali installati:

```powershell
./tests/network/RunNativeTest.ps1 -GameDirectory 'E:/Steam/steamapps/common/Penumbra Overture/redist' -Mode intro
./tests/network/RunNativeTest.ps1 -GameDirectory 'E:/Steam/steamapps/common/Penumbra Overture/redist' -Mode full
./tests/network/RunNativeTest.ps1 -GameDirectory 'E:/Steam/steamapps/common/Penumbra Overture/redist' -Mode full -OneWayDelayMs 35 -DropEvery 20
./tests/network/RunNativeTest.ps1 -GameDirectory 'E:/Steam/steamapps/common/Penumbra Overture/redist' -Mode hammer
./tests/network/RunNativeTest.ps1 -GameDirectory 'E:/Steam/steamapps/common/Penumbra Overture/redist' -Mode hammer -OneWayDelayMs 35 -DropEvery 20
```

Le prove creano profili isolati in `build/qa`, chiudono solo i propri processi e conservano log e screenshot. `intro` conserva gli script normali e avanza automaticamente i messaggi; `full` rimuove l'intro, posiziona il giocatore nelle fixture e verifica movimento, bottiglia, chiave, porta e cambio mappa. Questi script sono strumenti per sviluppatori; per giocare usare il launcher grafico.

`hammer` passa da cabina a esterno, attiva tramite fixture il cancello del passaggio e usa l'interazione nativa del portello per entrare nella miniera. Raccoglie il martello vero tramite richiesta del client, verifica entrambi gli equipaggiamenti e le torce, rompe due casse create da un asset originale con sette colpi nativi ciascuna e controlla i dieci frammenti sulla replica. Misura intervalli degli stati, correzioni e spostamenti per frame; non percorre manualmente la campagna.

Il codice originale genera ancora warning di conversione e usa `/Zc:forScope-` per alcuni vecchi cicli. Non è una compilazione senza warning. Le DLL runtime sono quelle della propria installazione originale e devono essere raggiungibili dalla directory di lavoro `redist`.

Per creare un pacchetto usare `tools/Package.ps1 -EvidenceFile <validation.json>`, dopo aver registrato le modifiche in Git. Lo script richiede prove positive associate agli hash dell'eseguibile e del launcher in `build/package-candidate`, archivia i sorgenti del commit e genera il manifest. Il file di prove va ricavato dalle esecuzioni effettive; lo script non esegue la campagna.
