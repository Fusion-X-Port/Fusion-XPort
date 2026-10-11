/*
 * Fusion X-Port — platform_desktop.c
 * Desktop device layer (Windows / macOS / Linux / Switch-in-SDL): the
 * shared SDL pump already delivers keyboard, gamepad buttons and mouse;
 * this layer adds analog-stick -> virtual-key derivation (circle-pad
 * equivalent) and the launcher folder convention.  Compiled only when
 * the 3DS layer is not.
 */
#include "platform_internal.h"

#if !defined(FXP_PLATFORM_3DS)

#include <SDL3/SDL.h>

#define FXP_PAD_THRESHOLD 0.30f     /* stick deflection for an arrow      */
#define FXP_PAD_TRIGGER   0.50f     /* ZL/ZR are 0/1 buttons via axes     */

void fxp_platform_device_init(void)
{
    /* Desktop controllers ride SDL's gamepad subsystem (3DS skips it —
     * the circle pad is read through libctru there). */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        fxp_log_warn("input: gamepad subsystem unavailable (%s)",
                     SDL_GetError());
}

void fxp_platform_device_exit(void)
{
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void fxp_platform_device_reset(void)
{
    /* the shared pump owns all desktop input state */
}

/* Analog sticks/triggers -> synthetic VK keys (the console path reads
 * the real analog hardware in platform_3ds.c instead). */
void fxp_platform_device_poll(fxp_platform_poll_t *out)
{
    const fxp_input_map_t *m = fxp_input_map_get();
    uint8_t *k = out->keys;

    const float up   = -FXP_PAD_THRESHOLD;
    const float down =  FXP_PAD_THRESHOLD;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_LEFTY)  < up ||
        fxp_sdl_axis(SDL_GAMEPAD_AXIS_RIGHTY) < up)    k[m->up] = 1;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_LEFTY)  > down ||
        fxp_sdl_axis(SDL_GAMEPAD_AXIS_RIGHTY) > down)  k[m->down] = 1;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_LEFTX)  < up ||
        fxp_sdl_axis(SDL_GAMEPAD_AXIS_RIGHTX) < up)    k[m->left] = 1;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_LEFTX)  > down ||
        fxp_sdl_axis(SDL_GAMEPAD_AXIS_RIGHTX) > down)  k[m->right] = 1;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > FXP_PAD_TRIGGER)
        k[m->btn_zl] = 1;
    if (fxp_sdl_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > FXP_PAD_TRIGGER)
        k[m->btn_zr] = 1;
}

const char *fxp_platform_device_name(void)
{
#if defined(FXP_PLATFORM_SWITCH)
    return "Nintendo Switch";
#elif defined(FXP_PLATFORM_WINDOWS)
    return "Windows";
#elif defined(FXP_PLATFORM_MACOS)
    return "macOS";
#else
    return "Linux";
#endif
}

const char *fxp_platform_device_games_dir(void)
{
    return "3ds/FusionXPort/games";
}

#else  /* FXP_PLATFORM_3DS — keep the translation unit non-empty */

typedef int fxp_platform_desktop_disabled;

#endif /* !FXP_PLATFORM_3DS */
