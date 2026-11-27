# Amnesia: The Bunker — Online Co-op (experimental)

Online co-op for Amnesia: The Bunker built only on the game's official modding
support: an **AddOn** (AngelScript) plus a small **bridge** process that does
the networking, because the game's script API has no sockets. Game files are
never modified. Design and plan: [docs/RAPPORTO_INIZIALE.md](docs/RAPPORTO_INIZIALE.md).

No game script is redistributed. The add-on hooks the game through its
`ModEventHandler` script, which it overrides by name; that copy is rebuilt at
install time from the player's own installation plus the six `[COOP]` lines in
[`mod/OnlineCoop/patches/ModEventHandler.coop.txt`](mod/OnlineCoop/patches/ModEventHandler.coop.txt).
Both `tools/InstallMod.ps1` and `BunkerCoop.exe` do this; the avatar animations
are copied from the installation the same way.

Status: phase 3 — game ↔ bridge channel and bridge ↔ bridge network work
(handshake, state both ways, reliable events, timeout and reconnect), plus a
launcher. Phase 4: the other player is visible (soldier avatar with the
player's own locomotion animations). Next: shared inventory (phase 5),
items held by the avatar.

Players: unzip the package from `tools/PackageBunker.ps1`, open Steam, run
`BunkerCoop.exe`, choose host or join. See `LEGGIMI.txt` in the package.

## Layout

| Path | What |
|---|---|
| `mod/OnlineCoop/` | the add-on copied into the game's `mods/` folder |
| `mod/OnlineCoop/patches/` | the `[COOP]` additions applied to the game's `ModEventHandler` at install time |
| `bridge/BridgeCore.cs` | game channel + UDP link (shared by launcher and command-line bridge) |
| `bridge/CoopBridge.cs` | command-line bridge, stand-in game, `--test-net` scripted network checks |
| `launcher/BunkerLauncher.cs` | `BunkerCoop.exe`: host/join, installs the add-on, runs the bridge, starts the game |
| `tools/BuildBridge.ps1`, `tools/BuildLauncher.ps1` | build with the csc shipped with Windows |
| `tools/PackageBunker.ps1` | player ZIP in `build/packages/` |
| `tools/InstallMod.ps1` | installs / updates / removes the add-on (backs up the previous copy) |
| `tools/LaunchWithMod.ps1` | starts the game with the add-on enabled, as the Mods menu does |
| `tools/CaptureLaunch.ps1` | records the command line the game uses to start with the add-on |
| `tools/RunTwoInstances.ps1` | two game instances + two bridges on one PC (avatar test) |
| `tools/MakePartnerEntity.py` | builds the avatar entity and the list of animations the installers copy from the game |

Avatar checks without a second player: `CoopBridge --join 127.0.0.1 --fake-walker
--ipc-dir <dir>` walks a loop in front of the real player. Arm poses can be
tuned live with `OnlineCoop/<profile>/avatar_tune.xml` (format in
`OnlineCoop_Avatar.hps`); without that file the built-in values apply.

## Try it

```powershell
./tools/BuildBridge.ps1
./build/CoopBridge.exe --test-net                                          # network checks, no game
./build/CoopBridge.exe --test-net --sim-loss 20 --sim-delay 150 --sim-jitter 80
./tools/InstallMod.ps1
./build/CoopBridge.exe --host                                              # leave it running
./build/CoopBridge.exe --join 127.0.0.1 --fake-game --ipc-dir $env:TEMP\fake   # a stand-in second player
```

Then either run `./tools/LaunchWithMod.ps1` (Steam must be open) or start the
game from Steam and pick **Storie personalizzate > Add-on > Online Co-op (experimental)**. In a
map, `game_msgs` counts up in the bridge window, and the game log
`Documents/My Games/Amnesia The Bunker/MainWithAddons/<hash>/hpl.log`
(readable after the game closes) has `[COOP/IPC]` lines with the round trip.
