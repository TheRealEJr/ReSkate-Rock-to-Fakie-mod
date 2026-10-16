### AFTER INSTALLING OPEN RESKATE -> ADVANCED -> UNCHECK INSTALL RESKATE UPDATES -> AND RESTART YOUR RESKATE
#### DO NOT HIT UPDATE IN THE BOTTOM RIGHT OR YOU WILL HAVE TO REINSTALL THE .bat AGAIN

# ReSkate Rock to Fakie mod

[![Version](https://img.shields.io/badge/version-v1.4.42-blue)](https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod)
[![Platform](https://img.shields.io/badge/platform-Windows_x64-lightgrey)](https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod)

Adds **Rock to Fakie** to the game's **original bottom-left trick ticker** during qualifying Board Stall and Lip Stall maneuvers in **skate.** using a modified **ReSkate.dll**. This is not the old center-screen overlay.

**Author / repository:** [TheRealEJR](https://github.com/TheRealEJR) · **Repository:** `TheRealEJR/ReSkate-Rock-to-Fakie-mod`

> Independent community mod, not affiliated with EA, Full Circle, or the ReSkate maintainers. Designed against the specific game and ReSkate executable/source used during development; not guaranteed compatible with every update. Use in offline/private sessions. **The git repository ships source, not a precompiled DLL.** A release with a DLL must be built and Windows-tested separately.

## Features

- Replaces the native **Board Stall** or **Lip Stall** localized name component with **Rock to Fakie** while the skater is in verified stall states `400/401`, using the previously validated call route and a size-checked output buffer.
- Keeps the HUD substitution enabled for the **entire current game session** rather than stopping after the old 45-second diagnostics capture.
- Supports the existing `9/12 → 187` recorder-copy trick-event remapping for verified B-triggered stall exits.
- Does not edit `Skate.exe`, localization database, input mappings, controller state, skating physics, or game saves.
- Leaves other trick names unchanged. A game's separate **BS/FS** prefix can still appear because the mod changes the localized stall-name component, not every part of the ticker.
- Includes installer and uninstaller that operate only on `ReSkate.dll`, preserve the previous version, and verify SHA-256 checksums before restoration.

## Requirements

- Windows x64 and a supported **skate.** / **ReSkate** setup (with `Skate.exe` and `ReSkate.dll` in the game directory).
- A **compiled v1.4.42 ReSkate.dll** from the modified sources. The repository does **not** contain a precompiled DLL and `install.bat` will refuse to run if the payload is absent.
- For building from source: a local ReSkate source checkout at the supported **v1.4.41** state or already-patched **v1.4.42**, configured for Visual Studio / CMake, Python 3, and the appropriate C++ toolchain.

## Download, install, and enable

1. Download a **compiled Windows release** from the [Releases page](https://github.com/TheRealEJR/ReSkate-Rock-to-Fakie-mod/releases), **once a tested release is published**. *The repository ZIP (Code → Download ZIP) is source-only.*
2. Close Skate and its launcher. Extract the compiled release ZIP.
3. Double-click `install.bat`; choose the directory containing `Skate.exe`, for example `D:\Skate (testing)`. Or run:

   ```bat
   install.bat -GameDir "D:\Skate (testing)"
   ```

4. After the game launches, open the ReSkate console and enter:

   ```text
   trainer rock auto off
   trainer rock trick hud
   trainer rock trick status
   ```

5. Perform a Board Stall / Lip Stall and press **physical Xbox B** to exit. Qualifying tricks will show **Rock to Fakie** in the original ticker. This remains enabled until disabled or the game is closed.

To turn it off without uninstalling:

```text
trainer rock trick off
```

**Note:** `trainer rock native 1` is no longer needed to activate the HUD. Telemetry capture still has its own time limit, but it does not turn off the v1.4.42 HUD mode. The HUD toggle resets to OFF when you restart Skate; re-enter `trainer rock trick hud` each session.

## Safe backup and uninstall

`install.bat` changes **only** the `ReSkate.dll` in your game folder. It saves the original beside it as:

```text
Skate game folder/
  Skate.exe                      (untouched)
  ReSkate.dll                    (Rock to Fakie build)
  RockToFakie_Backup/
    ReSkate.dll.original         (your previous DLL)
    installation.json            (verified file hashes)
```

The installer refuses to replace a conflicting prior backup and validates its DLL payload. To uninstall, close the game and run `uninstall.bat` from the *same extracted release folder*. It automatically remembers the previously selected game directory, or accepts:

```bat
uninstall.bat -GameDir "D:\Skate (testing)"
```

It verifies both the installed DLL and the backup with SHA-256 before restoring your old DLL. If you've updated or modified ReSkate after installing, the uninstaller **refuses to overwrite** that newer DLL; preserve `RockToFakie_Backup` and resolve the conflict manually. Don't delete that folder while the mod is installed.

If there was no previous DLL, uninstall removes only the mod's installed DLL. Other Skate and launcher files aren't affected.

## Build your own DLL

This is a **standalone mod source repository**, not a second full copy of the [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) project. Only the three modified source files are provided, along with the backup-first helper that applies them to a compatible ReSkate checkout:

```text
source_patch/payload/Extension/Skater/no_bail.cpp
source_patch/payload/Extension/Skater/no_bail.h
source_patch/payload/Extension/Trainer/trainer.cpp
```

## Known limitations

- The game may add a `BS` or `FS` prefix separately from the translated trick name.
- Text substitution is conditioned on stall state and the verified localization route; the event may be labeled differently under other game builds or mods.
- The trick-event remap is a **copied recorder event** for verified physical-B exits; it does not globally mutate the original producer or guarantee all game scoring/history paths use ID `187`.
- Support is tied to a particular game binary, and compilation on updated sources could fail.
- The HUD on/off state is per session, not stored persistently between launches.
- No Windows build or `.bat` end-to-end execution was performed in the preparation environment. Verify a compiled release on Windows before distributing it.

## Licensing and acknowledgments

The affected code is derived from [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate), which is licensed under **GNU GPL-3.0**. This repository includes the upstream GPL-3.0 `LICENSE`, retains the source modifications, and distinguishes this independent Rock to Fakie modification from the original ReSkate project. Any distributed build must comply with the upstream license and include corresponding source.
