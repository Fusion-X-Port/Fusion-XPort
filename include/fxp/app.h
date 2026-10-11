/*
 * Fusion X-Port — app.h
 * Application entry: runs the launcher (game list) and the selected game.
 */
#ifndef FXP_APP_H
#define FXP_APP_H

#include "fxp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the runtime.  Blocks until the user quits.
 *
 *   mfa_path != NULL — run that MFA directly (tests/tools/CLI).
 *   mfa_path == NULL — launcher mode: list the MFAs found in
 *                      "3dsFusionXPort/games/" (sdmc:/3dsFusionXPort/games
 *                      on the 3DS), A/Enter plays, START/Esc in a game
 *                      returns to the list, START/Esc in the list quits.
 *
 * Returns false only if the renderer could not be created; game load
 * failures return to the list with an on-screen notice instead.
 */
bool fxp_app_run(const char *mfa_path);

#ifdef __cplusplus
}
#endif
#endif /* FXP_APP_H */
