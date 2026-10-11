/*
 * Fusion X-Port — main.c
 * SDL3 entry point.  SDL3_main must be used on the 3DS so that ROMFS is
 * mounted before main() runs — hence SDL_main.h is included HERE, in the
 * translation unit that defines main (SDL's official N3DS note).
 *
 * Usage: FusionXPort [game.mfa]
 *   With no argument the launcher lists the MFAs found in the platform
 *   games folder (see fxp_platform_games_dir in source/platform/).
 *
 * Input note: every device lives behind fxp/platform.h — the desktop
 * layer uses SDL's keyboard/gamepad events, the 3DS layer polls libctru
 * directly (see source/platform/platform_3ds.c).  This file is
 * platform-neutral.
 */
#include "fxp/app.h"
#include "fxp/fxp.h"

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fxp_log_error("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    fxp_platform_init();
    fxp_log_info("Fusion X-Port %s — runtime for Clickteam Fusion MFA games",
                 FXP_VERSION_STR);
    int sdl_ver = SDL_GetVersion();     /* packed: MMmmmpp */
    fxp_log_info("platform: %s (SDL %d.%d.%d)", fxp_platform_name(),
                 sdl_ver / 1000000, (sdl_ver / 100000) % 100, sdl_ver % 1000);

    const char *path = argc > 1 ? argv[1] : NULL;

    bool ok = fxp_app_run(path);

    fxp_platform_quit();
    SDL_Quit();
    return ok ? 0 : 1;
}
