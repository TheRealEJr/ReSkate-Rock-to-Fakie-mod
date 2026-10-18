# IMPORTANT (TLDR)
   ```text
   rocktofakie on
   rocktofakie status
   ```
### AFTER INSTALLING OPEN RESKATE -> ADVANCED -> UNCHECK INSTALL RESKATE UPDATES -> AND RESTART YOUR RESKATE
#### DO NOT HIT UPDATE IN THE BOTTOM RIGHT OR YOU WILL HAVE TO REINSTALL THE .bat AGAIN

# The boring parts

## ReSkate Rock to Fakie mod

[![Version](https://img.shields.io/badge/version-v2.0.0-blue)](https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod)
[![ReSkate](https://img.shields.io/badge/ReSkate-2.1.0-orange)](https://github.com/Dingo-Shenanigans/ReSkate)
[![Platform](https://img.shields.io/badge/platform-Windows_x64-lightgrey)](https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod)

Adds **Rock to Fakie** to **skate.** through a modified **ReSkate.dll**. skate. has no button for a
rock to fakie, but the move is already in the game: ride up to a coping, Board Stall or Lip Stall on
it, and press **B** to roll back in fakie. With this mod on, the game calls it what it is.

**Author / repository:** [TheRealEJR](https://github.com/TheRealEJR) · `TheRealEJR/ReSkate-Rock-to-Fakie-mod`

> Independent community mod, not affiliated with EA, Full Circle, or the ReSkate maintainers. Built for
> **ReSkate v2.1.0** on skate. build `fbce74d5…` (the build ReSkate 2.1.0 supports). **This repository
> ships source, not a precompiled DLL.**

## Features

- **Trick ticker:** a Board Stall or Lip Stall shows as **Rock to Fakie** in the game's own bottom-left
  ticker while you hold it.
- **Trick record:** leaving the stall with **B** records the trick as the game's own `fakie_rock`
  (id 187) instead of `bs_board` (9) or `bs_lip` (12). The recorder gets a changed **copy** of the trick
  packet; the game's own packet is never written.
- **Off by default, remembered:** `rocktofakie on|off|status` in the ReSkate console. The setting is kept
  between sessions.
- Other tricks, other labels, other players' tickers, the string database, `Skate.exe`, input, physics
  and saves are untouched. Both game hooks check the game's code before installing and pass everything
  through unchanged while the mod is off.
- Installer and uninstaller change only `ReSkate.dll`, keep a SHA-256-verified backup of the original,
  and upgrade an earlier Rock to Fakie install in place.

## What's in this repository

```text
source_patch/payload/      the Rock to Fakie files for ReSkate v2.1.0 (new and changed files only)
source_patch/manifest.json SHA-256 of every file before and after the patch
apply_to_reskate.py        applies the patch to a ReSkate v2.1.0 checkout, with checks and backups
install.bat / uninstall.bat / scripts/manage.ps1   the game-folder installer
payload/                   where your built ReSkate.dll goes before running install.bat
```

New files: `Extension/Skater/rock_to_fakie.{h,cpp}`, `rock_to_fakie_commands.cpp`, `ROCK_TO_FAKIE.md`
and `Engine/Game/Build/20260929/rock_to_fakie.h`. Small changes wire it into ReSkate's startup, client
tick, console and build list, and add one field (`since`) to No Bail's `PhysicsStateWatch`.

## Build

Requirements: Windows 10/11 x64, **Visual Studio 2022 or 2026** (or Build Tools) with *Desktop
development with C++*, **Python 3**, **Git**. Optional: **Rust** ([rustup.rs](https://rustup.rs)) to
include ReSkate's Spotify device, YouTube Music, Local Files and Online Radio.

1. Get ReSkate v2.1.0 and this repository:

   ```powershell
   git clone --branch v2.1.0 https://github.com/Dingo-Shenanigans/ReSkate.git ReSkate-2.1.0
   git clone https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod.git
   ```

2. Apply the patch (check first, then apply):

   ```powershell
   python ReSkate-Rock-to-Fakie-mod\apply_to_reskate.py ReSkate-2.1.0 --dry-run
   python ReSkate-Rock-to-Fakie-mod\apply_to_reskate.py ReSkate-2.1.0
   ```

3. Open a **Developer PowerShell** (or load Visual Studio's tools into PowerShell) and build:

   ```powershell
   cd ReSkate-2.1.0
   cmake --preset vs2022-x64
   cmake --build --preset release --parallel 4
   ```

   With **Visual Studio 2026 only**, first change `"Visual Studio 17 2022"` to `"Visual Studio 18 2026"`
   in `CMakePresets.json` and remove its `"toolset": "v143"` line.

   For the music features, install Rust, then run `cargo build --release --lib` in `SpotifyHelper\`
   **before** the first `cmake --preset` (delete `build\` and configure again if you add Rust later).

4. The DLL is `build\vs2022-x64\Release\ReSkate.dll`.

## Install

1. Close Skate and the ReSkate launcher.
2. In the ReSkate launcher, open **Advanced** and **uncheck "Install ReSkate updates"**, then restart the
   launcher. Do not press **Update** in the bottom right: it replaces your DLL with the official one.
3. Copy your built `ReSkate.dll` into this repository's `payload\` folder.
4. Run `install.bat` and choose the folder that contains `Skate.exe`, or:

   ```bat
   install.bat -GameDir "D:\Skate"
   ```

5. Start Skate, open the ReSkate console (`~`) and enter:

   ```text
   rocktofakie on
   rocktofakie status
   ```

   Both parts should report **ready**.

Upgrading from v1.4.42: if that build is still the installed `ReSkate.dll`, `install.bat` replaces it and
keeps your original backup. The old `trainer rock ...` commands are not in v2.0.0.

## Uninstall

Close the game and run `uninstall.bat` from the same folder. It verifies the installed DLL and the backup
with SHA-256 and restores the DLL that was there before the first Rock to Fakie install. Then turn
"Install ReSkate updates" back on so the launcher brings ReSkate up to date. Do not delete
`RockToFakie_Backup` in the game folder while the mod is installed.

## Known limitations

- Tied to skate. build `fbce74d5…` and ReSkate v2.1.0. On another game build, `rocktofakie status`
  reports which part is unavailable.
- It does not detect real coping contact: any Board Stall or Lip Stall counts.
- The game may still add its own BS / FS prefix to the ticker label.
- Whether id 187 reaches every scoring and history path, and how a multiplayer host treats it, is
  unverified. The ticker label change is local only.

## Licensing and acknowledgments

The modified code is derived from [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate), licensed under
**GNU GPL-3.0**. This repository includes the GPL-3.0 `LICENSE`. Any distributed build must comply with
that license and include its corresponding source.
