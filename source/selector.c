/*
 * Fusion X-Port — selector.c
 * Object selection model: select-then-filter with safe deselection during
 * iteration and snapshot/restore for OR-ladders and child events.
 */
#include "fxp/selector.h"

static void ensure_cap(fxp_selector_t *sel, int32_t need)
{
    if (need <= sel->selected_cap)
        return;
    int32_t cap = sel->selected_cap ? sel->selected_cap * 2 : 16;
    while (cap < need)
        cap *= 2;
    sel->selected = fxp_realloc(sel->selected, (size_t)cap * sizeof(fxp_object_t *));
    sel->selected_cap = cap;
}

void fxp_selector_init(fxp_selector_t *sel, fxp_object_t **all_instances,
                       int32_t all_count, uint32_t object_info, bool is_qualifier)
{
    memset(sel, 0, sizeof(*sel));
    sel->object_info = object_info;
    sel->is_qualifier = is_qualifier;
    sel->all      = all_instances;
    sel->all_count = all_count;
    /* default selection: everything (generated runtimes Reset() before
     * first use; selecting all here keeps the model consistent) */
    fxp_selector_select_all(sel);
}

void fxp_selector_free(fxp_selector_t *sel)
{
    free(sel->selected);
    sel->selected      = NULL;
    sel->selected_count = 0;
    sel->selected_cap  = 0;
    /* the `all` list is allocated by the frame when the selector is
     * (re)built and belongs to the selector from then on */
    free(sel->all);
    sel->all = NULL;
    sel->all_count = 0;
}

void fxp_selector_reset(fxp_selector_t *sel, fxp_object_t *restrict_to)
{
    if (restrict_to) {
        /* NuclearRT Reset(restrictTo): only restricts when the instance
         * actually belongs to this selector */
        for (int32_t i = 0; i < sel->all_count; i++) {
            if (sel->all[i] == restrict_to) {
                sel->selected_count = 1;
                ensure_cap(sel, 1);
                sel->selected[0] = restrict_to;
                return;
            }
        }
    }
    fxp_selector_select_all(sel);
}

void fxp_selector_select_all(fxp_selector_t *sel)
{
    sel->selected_count = sel->all_count;
    if (sel->all_count <= 0 || !sel->all)
        return;
    ensure_cap(sel, sel->all_count);
    memcpy(sel->selected, sel->all, (size_t)sel->all_count * sizeof(fxp_object_t *));
}

void fxp_selector_select(fxp_selector_t *sel, fxp_object_t *obj)
{
    if (!obj)
        return;
    for (int32_t i = 0; i < sel->selected_count; i++)
        if (sel->selected[i] == obj)
            return;                     /* already selected */
    ensure_cap(sel, sel->selected_count + 1);
    sel->selected[sel->selected_count++] = obj;
}

void fxp_selector_select_only(fxp_selector_t *sel, fxp_object_t *obj)
{
    sel->selected_count = 1;
    ensure_cap(sel, 1);
    sel->selected[0] = obj;
}

void fxp_selector_deselect_at(fxp_selector_t *sel, int32_t index)
{
    if (index < 0 || index >= sel->selected_count)
        return;
    memmove(&sel->selected[index], &sel->selected[index + 1],
            (size_t)(sel->selected_count - index - 1) * sizeof(fxp_object_t *));
    sel->selected_count--;
}

void fxp_selector_add(fxp_selector_t *sel, fxp_object_t *obj)
{
    fxp_selector_select(sel, obj);
}

void fxp_selector_remove(fxp_selector_t *sel, fxp_object_t *obj)
{
    for (int32_t i = 0; i < sel->selected_count; i++) {
        if (sel->selected[i] == obj) {
            fxp_selector_deselect_at(sel, i);
            return;
        }
    }
}

int32_t fxp_selector_count(const fxp_selector_t *sel)
{
    return sel->selected_count;
}

fxp_object_t *fxp_selector_first(fxp_selector_t *sel)
{
    return sel->selected_count ? sel->selected[0] : NULL;
}

bool fxp_selector_save(const fxp_selector_t *sel, fxp_object_t ***out, int32_t *out_count)
{
    if (sel->selected_count == 0) {
        *out = NULL;
        *out_count = 0;
        return true;
    }
    fxp_object_t **snap = fxp_malloc((size_t)sel->selected_count * sizeof(fxp_object_t *));
    memcpy(snap, sel->selected, (size_t)sel->selected_count * sizeof(fxp_object_t *));
    *out = snap;
    *out_count = sel->selected_count;
    return true;
}

void fxp_selector_restore(fxp_selector_t *sel, fxp_object_t **objs, int32_t count)
{
    ensure_cap(sel, count);
    memcpy(sel->selected, objs, (size_t)count * sizeof(fxp_object_t *));
    sel->selected_count = count;
}

void fxp_selector_free_snapshot(fxp_object_t **objs)
{
    free(objs);
}

/* ------------------------------------------------------------------ iter -- */

void fxp_selector_iter_begin(fxp_selector_t *sel)
{
    sel->iter_index = 0;
    sel->iter_skip  = 0;
}

fxp_object_t *fxp_selector_iter_next(fxp_selector_t *sel)
{
    int32_t i = sel->iter_index + sel->iter_skip;
    sel->iter_skip = 0;
    if (i < 0 || i >= sel->selected_count)
        return NULL;
    sel->iter_index = i + 1;
    return sel->selected[i];
}

void fxp_selector_iter_deselect_current(fxp_selector_t *sel)
{
    /* the object we just returned was at iter_index-1 */
    fxp_selector_deselect_at(sel, sel->iter_index - 1);
    /* do not advance past the element that slid into our slot */
    if (sel->iter_index > 0)
        sel->iter_index -= 1;
    sel->iter_skip = 0;
}
