# RomFS (optional built-in game)

This folder is embedded into the .3dsx and mounted at `romfs:/` at runtime
(SDL3 mounts it automatically because we use SDL3_main).

If a `game.mfa` exists here AND the SD-card games folder is empty, the
launcher offers it as **"game (built-in)"**.

## The normal way to add games (no rebuild needed)

Copy your MFAs to the SD card — the standard 3DS homebrew folder:

    sdmc:/3ds/FusionXPort/games/*.mfa

(create the `FusionXPort/games` folders inside `sdmc:/3ds` if they do not
exist yet — the launcher also creates them on first run)

The launcher lists everything it finds there.  Controls in the list:
Up/Down select, A plays, START (or Esc) quits the program.  Inside a game
START (or Esc) returns to the list.
