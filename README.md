# Amnesia & Penumbra — Online Co-op

[![CI](https://github.com/MickSlash/amnesia-penumbra-online-coop/actions/workflows/ci.yml/badge.svg)](https://github.com/MickSlash/amnesia-penumbra-online-coop/actions/workflows/ci.yml)

Play **Amnesia: The Dark Descent** with a friend, **online**, through the whole original
campaign. Experimental two-player co-op built on the open-source HPL engines that
Frictional Games released under the GPL.

One player hosts and runs the real game: physics, monster AI, scripts and saves. The other
player joins, sends their input and sees the host's world replicated in real time, with
their own movement predicted locally so it does not feel laggy.

| Folder | Game | Engine | Status |
| --- | --- | --- | --- |
| [`amnesia/`](amnesia/README.md) | Amnesia: The Dark Descent | HPL2 (64-bit fork) | **Playable**, experimental |
| [`penumbra/`](penumbra/README.md) | Penumbra: Overture | HPL1 | Experimental 0.3 |
| [`bunker/`](bunker/README.md) | Amnesia: The Bunker | official add-on + bridge | Early work in progress, not playable yet |

> **No game files are included.** Both players need their own legitimate copy of the
> game (Steam or another edition). The packages contain only the mod executables.

## Amnesia: The Dark Descent

### What works

- The original campaign for two players, started fresh or continued from a save.
- Shared inventory, journal, notes and diaries; both players can use items.
- Physics objects, doors (including ones broken by monsters), levers, lamps, enemies,
  sounds, music, particles and scripted events are all replicated from the host.
- Each player has their own lantern, oil, health and sanity. Both can lean (Q/E), crouch,
  jump, run and grab objects.
- Saves are written on the host. Death and checkpoints are shared.
- Automatic reconnect after a short disconnect. If the host disappears for good, the
  client can take over from the last checkpoint it received.
- Optional Steam integration: Steam shows you as playing Amnesia, and from v0.6.0 the host
  can **invite a Steam friend** instead of sharing an IP (see *Connecting*; not yet tested
  between two real Steam accounts).
- Rejoining a running game: if the client drops or its game closes, the host pauses the
  campaign and the client can come back into the same game.

### How far it has been tested

- Two players have **finished the whole campaign** together over **Radmin VPN**. The
  issues they found along the way have been fixed, which is where most of the recent
  commits come from.
- Automated tests run two game instances on one PC: all 33 campaign maps load and
  synchronize on both sides, plus focused regression tests (doors, saves and inventory,
  deaths, level changes, network loss and delay).
- Hard mode and custom stories have not been tested online yet, and one playthrough
  cannot catch everything. Expect bugs; reports are welcome (see below).

### Requirements

- Windows 10 or 11, 64-bit, on both PCs.
- *Amnesia: The Dark Descent* installed on both PCs.
- OpenAL, which many games already install. If the game starts without sound or
  complains about `OpenAL32.dll`, install the free
  [OpenAL Windows installer](https://www.openal.org/downloads/) (`oalinst`).
- The **same mod package** on both PCs. A build with a different network protocol is
  refused at connection, so always update both.
- A way for the two PCs to reach each other: a Steam invite, or one of the options in
  *Connecting* below.

### Connecting

**Steam invite (new in v0.6.0, please test and report):** both players launch with
*Show Amnesia on Steam* ticked and Steam open. The host clicks **Host**, picks the friend
in the list at the bottom of the online menu and clicks **Invite**. The friend opens
`AmnesiaCoop.exe` **first**, then accepts the invite from Steam chat or uses **Join Game**
on the host in the Steam friends list. The connection goes through Steam's relays: no IP,
VPN or open port. Steam's own Play button starts the original game, which is why the
friend must open the mod before accepting. This has been tested on one PC with a
stand-in for Steam, not yet between two real Steam accounts.

Without Steam, the host's game listens on **UDP port 27020**. Any of these works:

- **[Radmin VPN](https://www.radmin-vpn.com/)** (used for the full playthrough): both join
  the same Radmin network; the client connects to the host's Radmin IP (`26.x.x.x`).
- **[Porthole](https://store.steampowered.com/app/4963920/Porthole_Local_Port_Sharing/)**
  (free on Steam, also tested): the host shares UDP port 27020 in a Porthole lobby, the
  client joins the lobby and accepts the port, then connects in game to `127.0.0.1`
  port `27020`. No router setup needed.
- Same LAN: use the host's local IP.
- Other VPNs such as Hamachi or ZeroTier should work the same way as Radmin (untested).
- Port forwarding: forward UDP 27020 on the host's router and use its public IP.

### Install and play

1. Download the ZIP from the [Releases](https://github.com/MickSlash/amnesia-penumbra-online-coop/releases)
   page and extract **all** of it to a new folder (not inside the game folder).
2. Run **`AmnesiaCoop.exe`**. The launcher is in English or Italian (menu at the top
   right). It finds the Steam install automatically; otherwise select your game folder.
3. Tick *Show Amnesia on Steam* only for the Steam edition, with Steam open.
4. Click **Launch Amnesia co-op**.
5. In the main menu choose **Online co-op**:
   - the host clicks **Host** (and can invite a Steam friend, see *Connecting*);
   - otherwise the other player types the host's IP and clicks **Connect**;
   - both click **Ready**;
   - the host picks **New campaign** or **Continue saved game**.

From v0.6.0 the online menu is in English, or in Italian when the game is set to Italian.
The game's own menus, subtitles and text stay in whatever language your game uses.
In fullscreen the launcher now uses your screen's native resolution; older versions
used 1280×720, which looked blurry on larger screens.

### Antivirus warnings

The executables are built from this repository and are not code-signed, so Windows
SmartScreen or an antivirus may flag them as unknown. They are not malware: you can
build them yourself from source (see below). The `manifest.json` in every package lists
the SHA-256 of each executable, so you can check that you got the same files as your
partner.

### Reporting problems

Each player's log is at `Documents\Amnesia\AmnesiaOnlineCoop_Host\hpl.log`. The game
overwrites it on the next start, so copy both logs (host and client) **before
restarting**, and open an issue saying which map and what happened. Lines starting with
`[NET/` describe the connection and are usually enough to find the cause.

### Known bugs

- **Flashbacks**: during flashbacks the colours look less saturated than in the original
  game.
- v0.6.0 reworked the client's movement and camera (rubber banding) and fixed levers,
  hand wheels and drawers for the client and its speed in flashbacks; a real game
  confirmed the rubber banding is mostly gone. v0.6.1 fixes the client's jump (it looked
  doubled), makes leaning with Q/E smooth and makes picking up items instant on the
  client. v0.6.2 lets the client hear its own footsteps, tinderbox, oil and other
  player sounds, and each player hears the partner's footsteps where the partner is.
  v0.6.3 removes the light flicker on the client in the final boss fight (Orb Chamber),
  smooths the client's view when crouching and when scripts turn the camera, and shows
  the client the same loading screen and quote as the host; both leave it together once
  both have loaded.
  These last fixes were measured in automated tests on one PC; please report how they
  feel in a real game.

### Known limits

- Two players only; Windows only.
- The host's PC runs the whole game. A slow host or a slow connection is felt by both.
- Progress since the last checkpoint can be lost if the host crashes.

## Penumbra: Overture

Experimental two-player co-op (version 0.3): shared inventory, host-authoritative enemies,
death and respawn, co-op checkpoints. UDP port 27120. The launcher and documentation are
currently in Italian; see [`penumbra/README.md`](penumbra/README.md).

## Amnesia: The Bunker

Early work in progress. It uses only the game's official modding support (an add-on) plus
a small bridge process for networking; game files are never modified. So far the partner
is visible and moves in the world; shared inventory is next. Not playable as co-op yet.
See [`bunker/README.md`](bunker/README.md).

## Building from source

**Amnesia** (Visual Studio 2022 or later, x64 C++ toolset):

```powershell
cd amnesia
Expand-Archive HPL2/dependencies.zip -DestinationPath HPL2 -Force   # once
msbuild amnesia/src/game/Lux.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
./tools/BuildLauncher.ps1
./tools/PackageOnlineCoop.ps1    # player ZIP in build/packages/
```

**Penumbra** (Win32 only, because the original Newton runtime is 32-bit):

```powershell
cd penumbra
./tools/Build.ps1 -Generator 'Visual Studio 17 2022' -Toolset v143
./tools/Package.ps1
```

With Visual Studio 2026 use the defaults (`v145`).

Developer documentation:

- `amnesia/docs/network_architecture.md`: protocol, authority, recovery and limits.
- `amnesia/docs/online_installazione_it.md`: installation and LAN/VPN guide (Italian).
- `amnesia/tests/network/`: protocol unit tests and the two-instance game tests
  (`RunGameSmoke.ps1`), which need a local copy of the game.
- `penumbra/BUILD.md`, `penumbra/TESTS.md`.

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml):

- **On every push to `main` and every pull request** it builds both mods and their
  launchers and runs the protocol and UDP transport tests (Amnesia and Penumbra) and the
  dedicated-server configuration tests on Linux. Binaries are kept as workflow artifacts.
- **On a `v*` tag** (for example `git tag v0.5.0 && git push origin v0.5.0`) it creates a
  **draft Release** with one ZIP per game and their SHA-256, to check and publish by hand.

Tests that run the real game need the original game data and stay local, for example:

```powershell
./amnesia/tests/network/RunGameSmoke.ps1 -GameDirectory '<Amnesia>' -Campaign
./amnesia/tests/network/RunImpairedNetworkTest.ps1 -GameDirectory '<Amnesia>' -StartMap 07_archives_cellar.map -Gate
./penumbra/tests/network/RunNativeTest.ps1 -GameDirectory '<Penumbra>/redist' -Mode hammer -OneWayDelayMs 35
```

## Credits

- **Frictional Games**: *Amnesia: The Dark Descent*, *Penumbra* and the HPL1/HPL2 engines,
  released as open source under the GPL. None of this exists without them.
- **[Amnesia64](https://github.com/buzer2020/Amnesia64)**: the 64-bit HPL2 fork the Amnesia
  mod is built on.
- **The Born**: the original local split-screen co-op mod for Amnesia that this online
  version grew from.
- **MickSlash**: online co-op for Amnesia, Penumbra and The Bunker.
- **Killen**: played the whole campaign with MickSlash to test it, and reported the bugs
  found along the way.

This is a fan project. It is not affiliated with or endorsed by Frictional Games.

## License

GPL-3.0-or-later, like the HPL1/HPL2 engines released by Frictional Games. Third-party
dependencies keep their own licenses (see each project's `LICENSE` and `THIRD_PARTY.md`).
Game assets are not included and are not covered by this license.
