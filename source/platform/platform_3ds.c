/*
 * Fusion X-Port — platform_3ds.c
 * Nintendo 3DS device layer: libctru hid polling (D-pad, buttons,
 * analog circle pad, New 3DS c-stick), the sdmc games-folder convention
 * and the console name.  Compiled ONLY when FXP_PLATFORM_3DS is set;
 * every other platform sees an empty translation unit.
 *
 * Input design (inherited from the v0.4-v0.8 app layer):
 *  - libctru is polled directly once per frame instead of going through
 *    SDL's gamepad subsystem.  SDL3's N3DS video driver also opens hid
 *    and scans it from its own event pump; libctru's hidInit/hidExit
 *    are ref-counted and hidScanInput is state-based, so both compose
 *    safely (verified against libctru services/hid.c).
 *  - circlePosition.dy is POSITIVE UP (screen-Y is inverted vs Fusion,
 *    where up = -Y), so the vertical comparisons deliberately mirror
 *    the horizontal ones — mapping dy<0 -> Up inverted the axis on
 *    hardware ("up goes down").
 *  - The c-stick is NOT part of hidKeysHeld: it is read through irrst
 *    with the same deadzone and sign convention.
 */
#include "platform_internal.h"

#if defined(FXP_PLATFORM_3DS)

#include <SDL3/SDL.h>
#include <3ds.h>

#define FXP_PAD_DEADZONE 40        /* raw circle-pad units (of 156)      */

static struct {
    bool hid_ok;
    bool irrst_ok;                    /* c-stick: New 3DS only             */
    u32  prev_held;
} g_3ds;

void fxp_platform_device_init(void)
{
    if (R_FAILED(hidInit()))
        return;
    /* C-stick needs irrst; optional — Old 3DS has no such service. */
    g_3ds.irrst_ok = R_SUCCEEDED(irrstInit());
    g_3ds.hid_ok = true;
    fxp_log_info("input: libctru hid polled directly (role map + analog "
                 "circle pad; SDL3 gamepad subsystem left uninitialized)");
}

void fxp_platform_device_exit(void)
{
    if (!g_3ds.hid_ok)
        return;
    if (g_3ds.irrst_ok)
        irrstExit();                  /* never exit an uninitialised svc  */
    hidExit();
    g_3ds.hid_ok = false;
}

void fxp_platform_device_reset(void)
{
    g_3ds.prev_held = 0;
}

void fxp_platform_device_poll(fxp_platform_poll_t *out)
{
    if (!g_3ds.hid_ok)
        return;

    hidScanInput();                   /* state-based: safe alongside SDL's pump */
    u32 held = hidKeysHeld();
    u32 down = held & ~g_3ds.prev_held;
    g_3ds.prev_held = held;

    const fxp_input_map_t *m = fxp_input_map_get();
    uint8_t *k = out->keys;

    if (held & KEY_DUP)    k[m->up]    = 1;
    if (held & KEY_DDOWN)  k[m->down]  = 1;
    if (held & KEY_DLEFT)  k[m->left]  = 1;
    if (held & KEY_DRIGHT) k[m->right] = 1;

    if (held & KEY_A)     k[m->btn1] = 1;
    if (held & KEY_B)     k[m->btn2] = 1;
    if (held & KEY_X)     k[m->btn3] = 1;
    if (held & KEY_Y)     k[m->btn4] = 1;
    if (held & KEY_L)     k[m->btn_l]  = 1;
    if (held & KEY_R)     k[m->btn_r]  = 1;
    if (held & KEY_ZL)    k[m->btn_zl] = 1;
    if (held & KEY_ZR)    k[m->btn_zr] = 1;
    if (held & KEY_SELECT) k[m->back] = 1;

    /* Circle pad (analog-only in libctru) with a small deadzone. */
    circlePosition cp;
    hidCircleRead(&cp);
    if (cp.dy >  FXP_PAD_DEADZONE) k[m->up]    = 1;
    if (cp.dy < -FXP_PAD_DEADZONE) k[m->down]  = 1;
    if (cp.dx < -FXP_PAD_DEADZONE) k[m->left]  = 1;
    if (cp.dx >  FXP_PAD_DEADZONE) k[m->right] = 1;

    /* C-stick (New 3DS): irrst, same sign convention as the circle pad. */
    if (g_3ds.irrst_ok) {
        irrstScanInput();
        circlePosition cs;
        irrstCstickRead(&cs);
        if (cs.dy >  FXP_PAD_DEADZONE) k[m->up]    = 1;
        if (cs.dy < -FXP_PAD_DEADZONE) k[m->down]  = 1;
        if (cs.dx < -FXP_PAD_DEADZONE) k[m->left]  = 1;
        if (cs.dx >  FXP_PAD_DEADZONE) k[m->right] = 1;
    }

    if (down & KEY_START)
        out->start_edge = true;
}

const char *fxp_platform_device_name(void)
{
    return "Nintendo 3DS";
}

const char *fxp_platform_device_games_dir(void)
{
    return "sdmc:/3ds/FusionXPort/games";
}

#else  /* !FXP_PLATFORM_3DS — keep the translation unit non-empty */

typedef int fxp_platform_3ds_disabled;

#endif /* FXP_PLATFORM_3DS */
