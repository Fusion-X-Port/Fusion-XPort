/*
 * Fusion X-Port — frame.c
 * Runtime frame: instance spawning, selectors, the NuclearRT update
 * pipeline (true events -> instance updates -> timers -> event loop).
 */
#include "fxp/frame.h"
#include "fxp/runtime.h"
#include "fxp/mfa_format.h"

static void ensure_cap(fxp_frame_t *f, int32_t need)
{
    if (need <= f->instance_cap)
        return;
    int32_t cap = f->instance_cap ? f->instance_cap * 2 : 32;
    while (cap < need)
        cap *= 2;
    f->instances = fxp_realloc(f->instances, (size_t)cap * sizeof(fxp_object_t *));
    f->instance_cap = cap;
}

fxp_frame_t *fxp_frame_create(const fxp_mfa_t *mfa, const fxp_mfa_frame_t *def)
{
    fxp_frame_t *f = fxp_calloc(1, sizeof(fxp_frame_t));
    f->mfa = mfa;
    f->def = def;
    f->next_handle = 1;
    f->rt = fxp_runtime_create(mfa, def);
    return f;
}

void fxp_frame_destroy(fxp_frame_t *frame)
{
    if (!frame)
        return;
    for (int32_t i = 0; i < frame->instance_count; i++)
        fxp_object_destroy(frame->instances[i]);
    free(frame->instances);
    for (int32_t i = 0; i < frame->selector_count; i++)
        fxp_selector_free(&frame->selectors[i]);
    free(frame->selectors);
    fxp_runtime_free(frame->rt);
    free(frame);
}

int32_t fxp_frame_spawn_instances(fxp_frame_t *frame)
{
    const fxp_mfa_frame_t *def = frame->def;
    int32_t created = 0;

    for (int32_t i = 0; i < def->instance_count; i++) {
        const fxp_mfa_instance_t *in = &def->instances[i];

        fxp_item_t *item = fxp_mfa_frame_item(def, (int32_t)in->item_handle);
        if (!item) {
            fxp_log_warn("frame '%s': instance %d references missing item %u",
                         def->name, i, in->item_handle);
            continue;
        }

        uint32_t handle = (in->handle >= 0) ? (uint32_t)in->handle
                                            : frame->next_handle;
        fxp_object_t *obj = fxp_object_create(item, handle);
        if (handle > frame->max_handle)
            frame->max_handle = handle;
        if ((uint32_t)in->handle >= frame->next_handle)
            frame->next_handle = in->handle + 1;
        frame->next_handle++;

        obj->x = in->x;
        obj->y = in->y;
        obj->layer = in->layer;
        obj->rgb_coefficient = item->transparent;
        obj->ink_effect = (uint8_t)item->ink_effect;
        obj->ink_effect_param = (uint8_t)item->ink_effect_param;

        /* visible-at-start flag from ObjectCommon NewFlags (bit 3) */
        obj->visible = (item->common.new_object_flags & (1 << 3)) != 0;

        fxp_object_init_movement(frame, obj);

        ensure_cap(frame, frame->instance_count + 1);
        frame->instances[frame->instance_count++] = obj;
        created++;
    }

    /* rebuild selectors (one per object info + one per qualifier) */
    for (int32_t i = 0; i < frame->selector_count; i++)
        fxp_selector_free(&frame->selectors[i]);
    free(frame->selectors);

    /* "EvOb" ObjectType==3 rows are qualifier references: conditions,
     * actions and expressions use their Handle as an object info. */
    int32_t qual_count = 0;
    for (int32_t i = 0; i < def->events.object_count; i++)
        if (def->events.objects[i].object_type == 3)
            qual_count++;

    frame->selector_count = def->item_count + qual_count;
    frame->selectors = fxp_calloc(
        (size_t)(frame->selector_count ? frame->selector_count : 1),
        sizeof(fxp_selector_t));

    for (int32_t i = 0; i < def->item_count; i++) {
        fxp_item_t *item = def->items[i];
        /* gather instances of this info */
        int32_t n = 0;
        for (int32_t j = 0; j < frame->instance_count; j++)
            if (frame->instances[j]->object_info == (uint32_t)item->handle)
                n++;
        fxp_object_t **list = fxp_malloc((size_t)(n ? n : 1) * sizeof(fxp_object_t *));
        int32_t k = 0;
        for (int32_t j = 0; j < frame->instance_count; j++)
            if (frame->instances[j]->object_info == (uint32_t)item->handle)
                list[k++] = frame->instances[j];
        fxp_selector_init(&frame->selectors[i], list, n,
                          (uint32_t)item->handle, false);
        /* note: list ownership stays with the selector's `all` array */
    }

    /* qualifier selectors (ObjectSelector.Initialize isQualifier): an
     * instance belongs when its item type matches and one of the item's
     * qualifier slots equals the qualifier id (masked, NuclearRT rule). */
    int32_t q = def->item_count;
    for (int32_t i = 0; i < def->events.object_count; i++) {
        const fxp_event_object_t *eo = &def->events.objects[i];
        if (eo->object_type != 3)
            continue;
        uint16_t qual_id = (uint16_t)(eo->system_qualifier & 0x7FFF);
        int32_t n = 0;
        for (int32_t j = 0; j < frame->instance_count; j++) {
            const fxp_object_t *obj = frame->instances[j];
            if (obj->type != (uint32_t)eo->item_type)
                continue;
            bool has = false;
            for (int32_t s = 0; s < 8; s++) {
                int16_t qv = obj->item->common.qualifiers[s];
                if ((qv & 0x7FFF) == (int16_t)qual_id && qv >= 0)
                    has = true;
            }
            if (has)
                n++;
        }
        fxp_object_t **list = fxp_malloc((size_t)(n ? n : 1) *
                                         sizeof(fxp_object_t *));
        int32_t k = 0;
        for (int32_t j = 0; j < frame->instance_count; j++) {
            fxp_object_t *obj = frame->instances[j];
            if (obj->type != (uint32_t)eo->item_type)
                continue;
            for (int32_t s = 0; s < 8; s++) {
                int16_t qv = obj->item->common.qualifiers[s];
                if ((qv & 0x7FFF) == (int16_t)qual_id && qv >= 0) {
                    list[k++] = obj;
                    break;
                }
            }
        }
        fxp_selector_init(&frame->selectors[q], list, n,
                          (uint32_t)eo->handle, true);
        q++;
    }

    fxp_log_info("frame '%s': spawned %d instance(s)", def->name, created);
    return created;
}

/* CreateInstance (Frame.cpp): new handle, position, selector add. */
fxp_object_t *fxp_frame_create_instance(fxp_frame_t *frame, fxp_item_t *item,
                                        int32_t x, int32_t y, int32_t layer)
{
    fxp_object_t *obj = fxp_object_create(item, frame->max_handle + 1);
    frame->max_handle++;
    obj->x = x;
    obj->y = y;
    obj->layer = (uint32_t)(layer < 0 ? 0 : layer);
    obj->created_this_tick = true;
    fxp_object_init_movement(frame, obj);

    ensure_cap(frame, frame->instance_count + 1);
    frame->instances[frame->instance_count++] = obj;
    return obj;
}

/* MarkForDeletion (Frame.cpp) — applied at end of tick. */
void fxp_frame_mark_for_deletion(fxp_frame_t *frame, fxp_object_t *obj)
{
    (void)frame;
    if (obj)
        obj->marked_for_delete = true;
}

/* DeleteMarkedInstances (Frame.cpp): remove from every selector and the
 * instance array (order-preserving for z-order). */
void fxp_frame_delete_marked(fxp_frame_t *frame)
{
    bool any = false;
    for (int32_t i = 0; i < frame->instance_count; i++)
        if (frame->instances[i]->marked_for_delete)
            any = true;
    if (!any)
        return;

    for (int32_t s = 0; s < frame->selector_count; s++) {
        fxp_selector_t *sel = &frame->selectors[s];
        for (int32_t i = 0; i < frame->instance_count; i++) {
            if (frame->instances[i]->marked_for_delete)
                fxp_selector_remove(sel, frame->instances[i]);
        }
        /* also drop from the `all` list */
        int32_t w = 0;
        for (int32_t i = 0; i < sel->all_count; i++)
            if (!sel->all[i]->marked_for_delete)
                sel->all[w++] = sel->all[i];
        sel->all_count = w;
    }

    int32_t w = 0;
    for (int32_t i = 0; i < frame->instance_count; i++) {
        if (frame->instances[i]->marked_for_delete) {
            fxp_object_destroy(frame->instances[i]);
            continue;
        }
        frame->instances[w++] = frame->instances[i];
    }
    frame->instance_count = w;
}

void fxp_frame_update(fxp_frame_t *frame, double dt)
{
    frame->timer += dt * 1000.0;
    if (frame->rt)
        frame->rt->time = frame->timer;

    /* per-instance movement + animation advance; queue "animation over"
     * edges (NuclearRT order: movements/animations, then events) */
    for (int32_t i = 0; i < frame->instance_count; i++) {
        fxp_object_movement_update(frame, frame->instances[i], dt);
        if (fxp_object_update(frame->instances[i], dt) && frame->rt)
            fxp_runtime_generate_event(frame, 2, -2, frame->instances[i]);
    }

    /* the full event pipeline */
    if (frame->rt)
        fxp_runtime_tick(frame, dt);

    /* deferred deletions (DeleteMarkedInstances) */
    fxp_frame_delete_marked(frame);
}

/* ==================================================== camera scroll ====== */

/* Layer coefficients (defaults 1.0 — scroll with the world). */
void fxp_frame_layer_coeffs(const fxp_frame_t *frame, uint32_t layer,
                            float *out_x, float *out_y)
{
    float cx = 1.0f, cy = 1.0f;
    if (frame && frame->def && layer < (uint32_t)frame->def->layer_count) {
        cx = frame->def->layers[layer].x_coefficient;
        cy = frame->def->layers[layer].y_coefficient;
        /* Fusion tolerates unset (0) coefficients as "plain layer"    */
        if (cx == 0.0f) cx = 1.0f;
        if (cy == 0.0f) cy = 1.0f;
    }
    if (out_x)
        *out_x = cx;
    if (out_y)
        *out_y = cy;
}

static int32_t scroll_clamp(int32_t v, int32_t frame_size, int32_t screen)
{
    int32_t max = frame_size - screen;
    if (max < 0)
        max = 0;                      /* frame smaller than the view     */
    if (v < 0)
        v = 0;
    if (v > max)
        v = max;
    return v;
}

/* NuclearRT Frame::SetScroll: the argument is the desired view CENTER;
 * layers with a coefficient > 1 scroll past the camera, so the request
 * is divided by the coefficient delta (parallax compensation). */
void fxp_frame_set_scroll(fxp_frame_t *frame, double cx, double cy,
                          int32_t layer)
{
    if (!frame || !frame->def)
        return;

    double rx = cx - (double)(FXP_SCREEN_W / 2);
    double ry = cy - (double)(FXP_SCREEN_H / 2);

    if (layer >= 0 && (uint32_t)layer < (uint32_t)frame->def->layer_count) {
        float lcx, lcy;
        fxp_frame_layer_coeffs(frame, (uint32_t)layer, &lcx, &lcy);
        if (lcx > 1.0f) {
            double dxf = rx - frame->scroll_x;
            rx = frame->scroll_x + dxf / (double)lcx;
        }
        if (lcy > 1.0f) {
            double dyf = ry - frame->scroll_y;
            ry = frame->scroll_y + dyf / (double)lcy;
        }
    }

    frame->scroll_x = (double)scroll_clamp((int32_t)rx,
                                           frame->def->size_x,
                                           FXP_SCREEN_W);
    frame->scroll_y = (double)scroll_clamp((int32_t)ry,
                                           frame->def->size_y,
                                           FXP_SCREEN_H);
}

void fxp_frame_set_scroll_x(fxp_frame_t *frame, double cx)
{
    if (!frame || !frame->def)
        return;
    int32_t rx = (int32_t)(cx - (double)(FXP_SCREEN_W / 2));
    frame->scroll_x = (double)scroll_clamp(rx, frame->def->size_x,
                                           FXP_SCREEN_W);
}

void fxp_frame_set_scroll_y(fxp_frame_t *frame, double cy)
{
    if (!frame || !frame->def)
        return;
    int32_t ry = (int32_t)(cy - (double)(FXP_SCREEN_H / 2));
    frame->scroll_y = (double)scroll_clamp(ry, frame->def->size_y,
                                           FXP_SCREEN_H);
}

fxp_object_t *fxp_frame_find_by_info(fxp_frame_t *frame, uint32_t object_info)
{
    for (int32_t i = 0; i < frame->instance_count; i++)
        if (frame->instances[i]->object_info == object_info)
            return frame->instances[i];
    return NULL;
}

fxp_selector_t *fxp_frame_selector(fxp_frame_t *frame, uint32_t object_info)
{
    for (int32_t i = 0; i < frame->selector_count; i++)
        if (frame->selectors[i].object_info == object_info)
            return &frame->selectors[i];
    return NULL;
}
