# Fusion X-Port

**A portable C runtime for Clickteam Fusion 2.5 games, built for the
hardware Fusion forgot.**  It plays games distributed as plain **MFA
project files** and currently on the **Nintendo 3DS**, with desktop builds
for development and more consoles planned.

---

## Runs MFAs, not EXEs

Fusion X-Port deliberately loads **only the plain, unencrypted MFA editor
format**.  There is no EXE/CCN unpacking and no decryption code anywhere
in this repository.  Authors export their project as an MFA (from
Clickteam Fusion's editor, copy it to an
SD card, and it runs.

## Features

### Runtime core (portable C99)

* **MFA loader** - the full editor format: MFU2 header, five banks,
  AutoUnicode strings, per-item zlib.  Tested against real Fusion-authored
  files (build 295) and CTFAK-produced exports.
* **Event interpreter** - ~100 conditions, ~100 actions and ~60 expressions, with groups, OR ladders, fast loops,
  Fusion timers, qualifiers, global values and counters.
* **Fusion arithmetic** - the expression evaluator keeps Clickteam's
  semantics (integer division truncates, `x/0 == 0`, infix source order).
* **Pixel-perfect collisions** - 1bpp masks built from image alpha, used when an object has *fine detection* enabled;
  box-mode objects keep the fast AABB path.

### Launcher

The runtime opens a **game list** of every `.mfa` in
the platform's games folder (`sdmc:/3ds/FusionXPort/games` on 3DS), plus an optional built-in game from
ROMFS.  Per-game, the console button map is rebuilt **from the game's own
player-control table**, so buttons keep the roles the author gave them.

## Quick start (Nintendo 3DS)

1. Install devkitPro with devkitARM and libctru:

   ```sh
   sudo dkp-pacman -S 3ds-dev 3ds-zlib
   ```

2. Build SDL3 for the 3DS (devkitPro does not package it yet; SDL3
   supports the 3DS natively):

   ```sh
   export DEVKITPRO=/opt/devkitpro
   ./tools/build-sdl3-3ds.sh
   ```

3. Build the runtime:

   ```sh
   make
   # -> FusionXPort.3dsx / .smdh
   ```

4. Copy your games to the SD card as `sdmc:/3ds/FusionXPort/games/*.mfa`
   (the folder is created on first launch).  Launch via the Homebrew
   Launcher, or `3dslink --address <3ds-ip> FusionXPort.3dsx`.

## Controls

| Input | Fusion role (with the default control table) |
|-------|----------------------------------------------|
| 3DS D-pad + circle pad / gamepad stick | Player 1 Up/Down/Left/Right (arrows) |
| 3DS A / B / X / Y | Button 1 (Shift) / 2 (Ctrl) / 3 (Space) / 4 (Return) |
| L / R, ZL / ZR | Buttons 5/6, 7/8 (Z, X, C, V) |
| SELECT / BACK | Escape — launcher back key |
| START | launcher key: game → list, list → quit |

## Where the format knowledge comes from

The MFA binary layouts were mapped by studying two open-source AGPL-3.0
projects (credited with gratitude; no code was copied — the C
implementation is independent):

* **NuclearRT** — 

a modern C++ Clickteam Fusion runtime.  Its architecture (application state machine,
  per-object-info selection model, value type, movement/animation engines)
  is mirrored by the runtime core.
* **CTFAK 2.0** — the Clickteam Fusion decompiler.  Its readers define *where everything lives inside an
  MFA*: chunk framing, bank layouts, frame items, object bodies and the
  event stream.  Every binary layout here was cross-checked field-by-field
  against its C# sources.

## Roadmap

1. **More consoles** - the platform layer makes a new target one
   `platform_<name>.c` plus a build entry: Switch (SDL3 supports it
   natively) first, then other retro platforms as backends permit.
2. **Movements** - path and platform movements.
3. **Object coverage** - counters (number sprites/bars), String, etc.
4. **Better Sound** - Sound is currently broken
