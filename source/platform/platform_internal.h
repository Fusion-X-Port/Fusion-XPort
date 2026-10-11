/*
 * Fusion X-Port — platform_internal.h
 * Internal contract between the shared SDL input pump (platform.c) and
 * the per-platform device layers (platform_3ds.c, platform_desktop.c).
 * NOT part of the public API — applications include fxp/platform.h.
 *
 * Exactly one device layer yields symbols per build (each file guards
 * its whole body with its platform macro), so builds may either link
 * only platform.c (which includes the matching layer) or compile every
 * platform_*.c and let the guards pick the right one.
 */
#ifndef FXP_PLATFORM_INTERNAL_H
#define FXP_PLATFORM_INTERNAL_H

#include "fxp/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The active button-role map (installed by fxp_input_init). */
const fxp_input_map_t *fxp_input_map_get(void);

/* Raw SDL stick/trigger state (-1..1, 0 when no gamepad), shared with
 * the device layers that derive virtual keys from the analog sticks. */
float fxp_sdl_axis(int axis);

/* --- implemented once per platform (platform_<name>.c) ------------------- */

/* Console/gamepad services beyond the shared SDL pump. */
void fxp_platform_device_init(void);
void fxp_platform_device_exit(void);

/* OR console-specific keys into out->keys, derive analog sticks, set
 * out->start_edge.  Runs after the shared SDL pump each frame. */
void fxp_platform_device_poll(fxp_platform_poll_t *out);

/* Console name for the boot banner. */
const char *fxp_platform_device_name(void);

/* Launcher folder for this platform. */
const char *fxp_platform_device_games_dir(void);

/* Latched-state clear (console prev-held snapshots, derived axes). */
void fxp_platform_device_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* FXP_PLATFORM_INTERNAL_H */
