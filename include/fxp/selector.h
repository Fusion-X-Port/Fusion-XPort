/*
 * Fusion X-Port — selector.h
 * Fusion's object selection model (per object-info instance lists).
 *
 * Ported from NuclearRT's ObjectSelector (runtime/include/ObjectSelector.h):
 * every condition runs as "select all, then filter"; conditions may
 * deselect during iteration; selections can be saved/restored around
 * OR-branches and child events.
 */
#ifndef FXP_SELECTOR_H
#define FXP_SELECTOR_H

#include "fxp.h"
#include "object.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxp_selector {
    fxp_object_t **selected;          /* owned array of borrowed refs     */
    int32_t        selected_count;
    int32_t        selected_cap;

    fxp_object_t **all;               /* every instance of this info      */
    int32_t        all_count;

    uint32_t       object_info;
    bool           is_qualifier;

    /* iteration state with deselect-during-iteration support */
    int32_t        iter_index;
    int32_t        iter_skip;
} fxp_selector_t;

void fxp_selector_init(fxp_selector_t *sel, fxp_object_t **all_instances,
                       int32_t all_count, uint32_t object_info, bool is_qualifier);
void fxp_selector_free(fxp_selector_t *sel);

/* Reset to the full instance list (or to a single origin instance when
 * restrict_to != NULL — NuclearRT's "trueEventSource" rule). */
void fxp_selector_reset(fxp_selector_t *sel, fxp_object_t *restrict_to);

void fxp_selector_select_all(fxp_selector_t *sel);
void fxp_selector_select(fxp_selector_t *sel, fxp_object_t *obj);
void fxp_selector_select_only(fxp_selector_t *sel, fxp_object_t *obj);
void fxp_selector_deselect_at(fxp_selector_t *sel, int32_t index);
void fxp_selector_add(fxp_selector_t *sel, fxp_object_t *obj);
void fxp_selector_remove(fxp_selector_t *sel, fxp_object_t *obj);
int32_t fxp_selector_count(const fxp_selector_t *sel);
fxp_object_t *fxp_selector_first(fxp_selector_t *sel);

/* Selection snapshots (for OR-ladders / child events). */
bool fxp_selector_save(const fxp_selector_t *sel, fxp_object_t ***out, int32_t *out_count);
void fxp_selector_restore(fxp_selector_t *sel, fxp_object_t **objs, int32_t count);
void fxp_selector_free_snapshot(fxp_object_t **objs);

/* Iteration: begin / next / deselect-current. */
void          fxp_selector_iter_begin(fxp_selector_t *sel);
fxp_object_t *fxp_selector_iter_next(fxp_selector_t *sel);
void          fxp_selector_iter_deselect_current(fxp_selector_t *sel);

#ifdef __cplusplus
}
#endif
#endif /* FXP_SELECTOR_H */
