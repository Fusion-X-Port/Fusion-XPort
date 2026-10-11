# Fusion X-Port

**A portable C runtime for Clickteam Fusion 2.5 games, built for the
hardware Fusion forgot.**  It plays games distributed as plain **MFA
project files** — currently on the **Nintendo 3DS**, with desktop builds
for development and more consoles planned.

![Platform](https://img.shields.io/badge/platform-Nintendo%203DS%20%7C%20Linux%20%7C%20Windows%20%7C%20macOS-blueviolet)
![Language](https://img.shields.io/badge/language-C99-lightgrey)
![License](https://img.shields.io/badge/license-AGPL--3.0-green)

---

## Runs MFAs, not EXEs

Fusion X-Port deliberately loads **only the plain, unencrypted MFA editor
format**.  There is no EXE/CCN unpacking and no decryption code anywhere
in this repository.  Authors export their project as an MFA (from
Clickteam Fusion's editor, or via CTFAK's *Export as MFA*), copy it to an
SD card, and it runs — nobody needs pirated executables to play.

## Features

### Runtime core (portable C99)

* **MFA loader** — the full editor format: MFU2 header, five banks,
  AutoUnicode strings, per-item zlib.  Tested against real Fusion-authored
  files (build 295) and CTFAK-produced exports.
* **Event interpreter** — NuclearRT-style select-then-filter engine:
  ~100 conditions, ~100 actions and ~60 expressions (complete list in
  [docs/EVENTS.md](docs/EVENTS.md)), with groups, OR ladders, fast loops,
  Fusion timers, qualifiers, global values and counters.
* **Fusion arithmetic** — the expression evaluator keeps Clickteam's
  semantics (integer division truncates, `x/0 == 0`, infix source order).
* **Movements** — Static, **Eight Directions** (acceleration/deceleration,
  32-dial direction mask) and **Bouncing Ball** (256-tick Sinus32/Cosinus32
  fixed-point tables, deceleration, angle snapping, Randomizer), plus the
  movement events: Bounce, Set speed, Set direction, Stop/Start.
* **Pixel-perfect collisions** — 1bpp masks built from image alpha
  (NuclearRT layout), used when an object has *fine detection* enabled;
  box-mode objects keep the fast AABB path.
* **Camera scroll** — actions *Center display at position / at X / at Y*
  move a real camera with NuclearRT's exact clamp contract
  (`scroll = clamp(center − screen/2, 0, frame − screen)`), per-layer
  scroll coefficients (1.0 world · 0.0 HUD · 2.0+ parallax) and
  camera-relative edge conditions/expressions.
* **Bouncing balls bounce off obstacles, not frame borders** — faithful to
  Fusion: a ball flies straight off the frame edge; walls are made of
  obstacle-flagged backdrop/quick-backdrop objects, which is what the
  movement reflects off.  (An earlier version bounced off frame borders —
  that was a misconception and was removed.)

### Audio

* **Sound bank** — WAV (u8/s16/s24/s32/float) played directly; **MP3
  (dr_mp3)** and **OGG (stb_vorbis)** decoded to 16-bit PCM in memory.
* **Music bank** — `ASUM` items dispatch through the same mixer.
* Per-channel resampling (linear interpolation), saturation-safe mixing,
  and NuclearRT's sample-action table (play / play-N / stop codes).

### Platform layer

Everything that touches real hardware lives behind one header —
[`include/fxp/platform.h`](include/fxp/platform.h) — implemented once per
platform in `source/platform/`:

| File | Role |
|------|------|
| `platform.c` | shared SDL3 event pump: keyboard/gamepad/mouse → Fusion VK space |
| `platform_3ds.c` | libctru `hid` polling, circle pad, c-stick (`irrst`), SD folder |
| `platform_desktop.c` | analog-stick derivation, desktop conventions |

The core runtime and the application layer contain **zero** console code —
adding a platform is one new `platform_<name>.c` (five `fxp_platform_device_*`
hooks) plus a mark in `fxp.h`.  See
[`source/platform/platform_internal.h`](source/platform/platform_internal.h)
for the contract.

### Launcher

With no MFA argument the runtime opens a **game list** of every `.mfa` in
the platform's games folder (`sdmc:/3ds/FusionXPort/games` on 3DS,
`./3ds/FusionXPort/games` on desktop), plus an optional built-in game from
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

> **Upgrading from v0.8 or earlier?**  Extract this release into a **fresh
> directory** (or delete the old tree first).  Old layouts leave a stale
> `source/platform.c` behind, which now collides with `source/platform/` —
> the Makefile detects this and tells you exactly what to remove instead
> of failing deep inside the linker.

## Quick start (desktop)

```sh
make -f Makefile.host            # -> build_host/fxp_host (needs SDL3 + zlib)
./build_host/fxp_host game.mfa   # or with no argument: the launcher
make -f Makefile.host test-core  # full headless test battery (no SDL3 needed)
```

The host build exercises exactly the same core code the 3DS build runs —
only the SDL3 window creation differs (2x-scaled 400x240 top screen).

## Controls

| Input | Fusion role (with the default control table) |
|-------|----------------------------------------------|
| 3DS D-pad + circle pad / gamepad stick | Player 1 Up/Down/Left/Right (arrows) |
| 3DS A / B / X / Y | Button 1 (Shift) / 2 (Ctrl) / 3 (Space) / 4 (Return) |
| L / R, ZL / ZR | Buttons 5/6, 7/8 (Z, X, C, V) |
| SELECT / BACK | Escape — launcher back key |
| START | launcher key: game → list, list → quit |
| Keyboard (desktop) | direct Windows-VK map (arrows, letters, F1–F12…) |
| Mouse (desktop) | mouse conditions |

Why the 3DS is polled through libctru directly (and why that composes
safely with SDL3's own N3DS driver): see
[docs/CHANGELOG.md](docs/CHANGELOG.md) (v0.7/v0.9 entries) and the comments
in `source/platform/platform_3ds.c`.

## Project layout

```
fusion-xport/
├── Makefile               devkitPro 3DS build (official template)
├── Makefile.host          desktop development build (SDL3 + zlib)
├── include/fxp/           public headers (platform.h = THE platform API)
├── source/
│   ├── byte_reader.c      endian-safe reads, UTF-16 -> UTF-8
│   ├── value.c            Clickteam arithmetic semantics
│   ├── selector.c         selection with deselect-during-iteration
│   ├── object.c           instance state, animation advance
│   ├── frame.c            instance spawning, camera, per-tick pipeline
│   ├── event_runtime.c    the event interpreter (NuclearRT semantics)
│   ├── movement.c         static / eight-directions / bouncing-ball engines
│   ├── mfa_reader.c       the MFA loader (header/banks/frames)
│   ├── mfa_objects.c      frame item loaders (Active/Backdrop/Text/…)
│   ├── mfa_events.c       event groups/conditions/actions/expressions
│   ├── mfa_image.c        graphic modes 4/6/7/8 + zlib (mode 4 = B,G,R)
│   ├── audio_decode.c     WAV/MP3/OGG -> 16-bit PCM
│   ├── app.c              launcher + state machine + main loop
│   ├── render_sdl3.c      SDL3 renderer backend
│   ├── audio_sdl3.c       SDL3 audio backend
│   ├── main.c             SDL3 entry (SDL_main.h — required for 3DS ROMFS)
│   └── platform/          THE platform boundary
│       ├── platform.c          shared SDL3 input pump + lifecycle
│       ├── platform_3ds.c      libctru hid/circle pad/c-stick (3DS only)
│       └── platform_desktop.c  sticks/gamepads elsewhere (3DS: not built)
├── tests/                 headless regression suites (see Testing)
├── tools/                 fixture builders, MFA dumper, SDL3-3DS build script
├── docs/                  EVENTS reference + changelog
└── romfs/                 optional built-in game.mfa
```

## Testing

`make -f Makefile.host test-core` runs the headless battery — ~150 checks
plus the real-game e2e — on machines without SDL3:

* `test_mfa_reader` — synthetic MFA round-trip (loader invariants)
* `make_input_mfa` + `test_phase0` — input events, qualifiers, overlaps,
  click edges, phase-0 regressions
* `test_user_game` — plays a real Fusion-authored MFA headlessly: D-pad
  movement, gem collection, alterable values, the win-condition frame jump
  (asserted as a frame *handle*), sample loading/playback
* `test_movement` — movement engines: speed scaling, direction masks,
  bouncing-ball reflection off obstacle backdrops (and *through* frame
  borders), angle snapping, deceleration
* `test_scroll` — camera clamp contract, coefficient compensation,
  actions 7/8/9 through the interpreter, edge expressions
* `test_audio_decode` — WAV synthesis round-trip, MP3/OGG decode

With SDL3 installed, `make -f Makefile.host test` adds the app-level
pixel e2e (hold-a-key → snapshot diff), and everything is ASan/UBSan clean
under `-fsanitize=address,undefined`.

## Where the format knowledge comes from

The MFA binary layouts were mapped by studying two open-source AGPL-3.0
projects (credited with gratitude; no code was copied — the C
implementation is independent):

* **[NuclearRT](https://github.com/ctfakteam/NuclearRT)** — a modern C++
  Clickteam Fusion runtime.  Its architecture (application state machine,
  per-object-info selection model, value type, movement/animation engines)
  is mirrored by the runtime core.
* **[CTFAK 2.0](https://github.com/ctfakteam/CTFAK2.0)** — the Clickteam
  Fusion decompiler.  Its readers define *where everything lives inside an
  MFA*: chunk framing, bank layouts, frame items, object bodies and the
  event stream.  Every binary layout here was cross-checked field-by-field
  against its C# sources.

## Roadmap

1. **More consoles** — the platform layer makes a new target one
   `platform_<name>.c` plus a build entry: Switch (SDL3 supports it
   natively) first, then other retro platforms as backends permit.
2. **Movements** — path and platform movements.
3. **Object coverage** — counters (number sprites/bars), Question/RTF.
4. **Sound channels** — Fusion channel semantics (lock, pan, volume).

## License

AGPL-3.0 — see [LICENSE](LICENSE).  Fusion X-Port is an independent
implementation inspired by the published research of the NuclearRT and
CTFAK teams.  Bundled third-party decoders keep their own licenses
(`stb_vorbis`, `dr_mp3` — public domain / MIT-style; see
`source/thirdparty/README.md`).
