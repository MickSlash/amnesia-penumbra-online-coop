# Amnesia & Penumbra — Online Co-op

[![CI](https://github.com/MickSlash/amnesia-penumbra-online-coop/actions/workflows/ci.yml/badge.svg)](https://github.com/MickSlash/amnesia-penumbra-online-coop/actions/workflows/ci.yml)

Mod sperimentali per giocare **in due online** ai titoli Frictional Games, costruite sui
motori open source HPL. L'host è autorevole (fisica, IA, script, salvataggi); il client
invia gli input e mostra il mondo replicato, con prediction del proprio movimento.

| Cartella | Gioco | Motore | Piattaforma |
| --- | --- | --- | --- |
| [`amnesia/`](amnesia/README.md) | Amnesia: The Dark Descent | HPL2 (fork 64 bit) | Windows x64 |
| [`penumbra/`](penumbra/README.md) | Penumbra: Overture | HPL1 | Windows x86 |

Serve una copia legittima del gioco su **entrambi** i PC: il repository e i pacchetti
non contengono mappe, texture, audio o DLL commerciali.

## Struttura

Ogni cartella è un progetto autonomo con la sua storia Git (importata con `git subtree`),
la sua documentazione e i suoi script:

- `amnesia/docs/network_architecture.md` — protocollo, autorità, recupero e limiti.
- `amnesia/docs/online_installazione_it.md` — installazione e gioco via LAN/VPN.
- `penumbra/README.md`, `penumbra/BUILD.md`, `penumbra/TESTS.md` — guida, build e verifiche.

## Compilare

**Amnesia** (Visual Studio 2022 o successivo, toolset C++ x64):

```powershell
cd amnesia
Expand-Archive HPL2/dependencies.zip -DestinationPath HPL2 -Force   # una volta
msbuild amnesia/src/game/Lux.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
./tools/BuildLauncher.ps1
```

**Penumbra** (Win32 obbligatorio, il runtime Newton originale è a 32 bit):

```powershell
cd penumbra
./tools/Build.ps1 -Generator 'Visual Studio 17 2022' -Toolset v143
```

Con Visual Studio 2026 si usano i valori predefiniti (`v145`). I pacchetti completi si
creano in locale con `amnesia/tools/PackageOnlineCoop.ps1` e `penumbra/tools/Package.ps1`.

## CI/CD

[`.github/workflows/ci.yml`](.github/workflows/ci.yml):

- **A ogni push su `main` e a ogni pull request** compila entrambe le mod e i launcher, esegue
  i test di protocollo e trasporto UDP (Amnesia e Penumbra) e i test del validatore di
  configurazione del server dedicato su Linux. I binari restano come artifact del workflow.
- **Con un tag `v*`** (per esempio `git tag v0.4.0 && git push origin v0.4.0`) crea una
  **Release in bozza** con uno zip per gioco e i relativi SHA-256, da verificare e pubblicare
  a mano.

Le prove native (due istanze del gioco reale, relay UDP con ritardo e perdita) richiedono i
dati originali e restano locali, per esempio:

```powershell
./amnesia/tests/network/RunImpairedNetworkTest.ps1 -GameDirectory '<Amnesia>' -StartMap 07_archives_cellar.map -Gate
./penumbra/tests/network/RunNativeTest.ps1 -GameDirectory '<Penumbra>/redist' -Mode hammer -OneWayDelayMs 35
```

## Licenza

GPL-3.0-or-later, come i motori HPL1/HPL2 rilasciati da Frictional Games. Le dipendenze di
terze parti mantengono le proprie licenze (vedi i file `LICENSE`/`THIRD_PARTY.md` di ciascun
progetto).
