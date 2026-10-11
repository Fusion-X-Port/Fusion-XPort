/*
 * Fusion X-Port — platform.h
 * The boundary between the portable runtime and the machine it runs on.
 *
 * Everything that actually touches console hardware (libctru on the 3DS,
 * desktop keyboards/gamepads via SDL, future: Vita/Switch/Wii inputs,
 * custom home button handling, SD folder conventions, ...) lives behind
 * this header.  The core runtime (event interpreter, movements, frames)
 * and the application layer (app.c) NEVER include console headers —
 * they only talk to the API below, which is implemented once per
 * platform in source/platform/ (platform_3ds.c, platform_desktop.c).
 *
 * Adding a platform:
 *   1. add a FXP_PLATFORM_<NAME> mark in fxp.h,
 *   2. write source/platform/platform_<name>.c implementing the five
 *      fxp_platform_device_* hooks (see platform_internal.h),
 *   3. include it from the build (its body is guarded by the platform
 *      macro, so every other platform compiles it as an empty unit).
 *
 * Copyright (C) 2025 Fusion X-Port contributors
 * Licensed under the GNU Affero General Public License v3.0.
 */
#ifndef FXP_PLATFORM_H
#define FXP_PLATFORM_H

#include "fxp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------- console button roles --
 *
 * Fusion events are authored against the *control table* stored inside
 * each MFA (Player 1..4 -> Up/Down/Left/Right/Button1..12, each bound to
 * a Windows VK code).  Console buttons therefore map to those ROLES, not
 * to fixed keys: the application rebuilds this map from the loaded
 * game's own control table whenever a game starts (falling back to
 * Fusion's default bindings for empty entries) and hands it to the
 * platform through fxp_input_init().  The platform then reports input
 * in Windows-VK space, which is what the runtime understands.
 *
 *   D-pad / circle pad -> Player 1 Up/Down/Left/Right (default: arrows)
 *   3DS A              -> Player 1 Button 1 (default: Shift)
 *   3DS B              -> Player 1 Button 2 (default: Ctrl)
 *   3DS X              -> Player 1 Button 3 (default: Space)
 *   3DS Y              -> Player 1 Button 4 (default: Return)
 *   L / R              -> Player 1 Button 5 / 6 (default: Z / X)
 *   ZL / ZR            -> Player 1 Button 7 / 8 (default: C / V)
 *   SELECT             -> back    (launcher "back" key)
 *   START              -> menu    (launcher quit / leave current game)
 */
typedef struct fxp_input_map {
    uint8_t up, down, left, right;
    uint8_t btn1, btn2, btn3, btn4;
    uint8_t btn_l, btn_r, btn_zl, btn_zr;
    uint8_t back;                     /* SELECT / Escape               */
} fxp_input_map_t;

/* Everything the platform observed between two polls.  `keys` is a
 * 256-entry Windows-VK bitmap (OR of every input device); edges are
 * derived by the caller by comparing against the previous poll. */
typedef struct fxp_platform_poll {
    uint8_t keys[256];                /* held, in VK space              */
    bool    start_edge;               /* START/menu went down now       */
    bool    quit;                     /* window close / OS terminate    */
    bool    mouse_down;               /* any pointer button held        */
    bool    mouse_clicked;            /* pointer pressed now            */
    int32_t mouse_x, mouse_y;         /* pointer position (screen px)   */
} fxp_platform_poll_t;

/* ------------------------------------------------------------- lifecycle --
 *
 * Name/reporting hooks (safe to call any time).
 */
const char *fxp_platform_name(void);

/* One-time init/quit around the whole program (console services that
 * are not input-specific go here). */
void        fxp_platform_init(void);
void        fxp_platform_quit(void);

/* Launcher folder convention for this platform:
 *   3DS     sdmc:/3ds/FusionXPort/games
 *   desktop ./3ds/FusionXPort/games
 */
const char *fxp_platform_games_dir(void);

/* ------------------------------------------------------------------ input --
 *
 * `fxp_input_init` (re)installs the button-role map; call it once at
 * boot and again whenever a new game loads.  `fxp_input_reset` clears
 * latched device state (use when returning to the launcher so a held
 * key does not leak into the list).  `fxp_input_poll` pumps every
 * device once per frame — call it exactly once per frame tick.
 */
void fxp_input_init(const fxp_input_map_t *map);
void fxp_input_reset(void);
void fxp_input_exit(void);
void fxp_input_poll(fxp_platform_poll_t *out);

#ifdef __cplusplus
}
#endif
#endif /* FXP_PLATFORM_H */
