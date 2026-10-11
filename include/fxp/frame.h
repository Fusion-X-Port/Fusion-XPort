/*
 * Fusion X-Port — frame.h
 * Runtime frame: spawns instances from an MFA frame, owns the selection
 * model and drives the per-tick update pipeline.
 *
 * Pipeline (mirrors NuclearRT Frame::Update):
 *   true events -> per-instance updates -> event loop -> deferred deletes
 */
#ifndef FXP_FRAME_H
#define FXP_FRAME_H

#include "fxp.h"
#include "mfa.h"
#include "object.h"
#include "selector.h"

/* fxp_runtime_t (runtime.h) — forward declared to break the cycle */
typedef struct fxp_runtime fxp_runtime_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxp_frame {
    const fxp_mfa_t       *mfa;       /* owning application (not owned)   */
    const fxp_mfa_frame_t *def;

    fxp_object_t **instances;
    int32_t        instance_count;
    int32_t        instance_cap;
    uint32_t       next_handle;
    uint32_t       max_handle;        /* CreateInstance handle counter     */

    double scroll_x, scroll_y;
    double timer;                     /* milliseconds since frame start   */
    fxp_color_t background_override;  /* Set background color action      */
    bool        has_background_override;

    /* one selector per object info (indexed like def->items) */
    fxp_selector_t *selectors;
    int32_t         selector_count;

    /* event interpreter state */
    fxp_runtime_t *rt;
    bool           at_eof;            /* evaluating end-of-frame pass     */
} fxp_frame_t;

fxp_frame_t *fxp_frame_create(const fxp_mfa_t *mfa, const fxp_mfa_frame_t *def);
void         fxp_frame_destroy(fxp_frame_t *frame);

/* Instantiate every MFA instance whose parent machinery we understand.
 * Returns the number of instances created. */
int32_t fxp_frame_spawn_instances(fxp_frame_t *frame);

/* One tick of the runtime: per-instance animation update + the full
 * NuclearRT event pipeline (true events -> timers -> normal -> EOF). */
void fxp_frame_update(fxp_frame_t *frame, double dt);

/* --- camera scroll (Frame::SetScroll / UpdateScrolling) -------------------
 *
 * The frame keeps a camera (scroll_x/scroll_y) as the FRAME coordinate
 * of the top-left visible pixel; instances stay in frame coordinates
 * and every consumer (renderer, screen-edge conditions, X/Y-edge
 * expressions) works against the camera.  The "center display" events
 * (app actions 7/8/9) feed the desired CENTER of the view; the runtime
 * subtracts half the viewport and clamps to the frame like NuclearRT:
 *
 *     scroll = clamp(center - screen/2, 0, frameSize - screenSize)
 *
 * Layer scroll coefficients (MFA layer XCoefficient/YCoefficient) are
 * applied by the renderer through fxp_frame_layer_coeffs: 1.0 scrolls
 * with the world (default), 0.0 pins a layer to the screen (HUD), 2.0
 * parallaxes at double speed.
 */

/* Center the view at frame coordinates (cx, cy); `layer` >= 0 applies
 * that layer's coefficient compensation for >1 coefs (NuclearRT rule),
 * pass -1 for absolute scrolling. */
void fxp_frame_set_scroll(fxp_frame_t *frame, double cx, double cy,
                          int32_t layer);
void fxp_frame_set_scroll_x(fxp_frame_t *frame, double cx);
void fxp_frame_set_scroll_y(fxp_frame_t *frame, double cy);

/* Scroll coefficients of `layer` (1.0 when the layer is unknown or the
 * frame has no layer list). */
void fxp_frame_layer_coeffs(const fxp_frame_t *frame, uint32_t layer,
                            float *out_x, float *out_y);

fxp_object_t *fxp_frame_find_by_info(fxp_frame_t *frame, uint32_t object_info);
fxp_selector_t *fxp_frame_selector(fxp_frame_t *frame, uint32_t object_info);

/* Runtime object management (used by the event interpreter):
 * create adds the instance to the frame + its selector, destroy marks
 * for deletion (applied at end of tick), matching NuclearRT's
 * CreateInstance / MarkForDeletion / DeleteMarkedInstances. */
fxp_object_t *fxp_frame_create_instance(fxp_frame_t *frame, fxp_item_t *item,
                                        int32_t x, int32_t y, int32_t layer);
void fxp_frame_mark_for_deletion(fxp_frame_t *frame, fxp_object_t *obj);
void fxp_frame_delete_marked(fxp_frame_t *frame);

#ifdef __cplusplus
}
#endif
#endif /* FXP_FRAME_H */
