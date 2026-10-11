/*
 * Fusion X-Port — object.c
 */
#include "fxp/object.h"
#include "fxp/mfa_format.h"

static void grow_values(fxp_object_t *obj, int32_t index)
{
    if (index < obj->value_count)
        return;
    int32_t old = obj->value_count;
    int32_t n = obj->value_count ? obj->value_count : 8;
    while (n <= index)
        n *= 2;
    obj->values = fxp_realloc(obj->values, (size_t)n * sizeof(fxp_value_t));
    for (int32_t i = old; i < n; i++)
        fxp_value_init_int(&obj->values[i], 0);
    obj->value_count = n;
}

fxp_object_t *fxp_object_create(const fxp_item_t *item, uint32_t handle)
{
    fxp_object_t *obj = fxp_calloc(1, sizeof(fxp_object_t));
    obj->item        = item;
    obj->type        = (uint32_t)item->object_type;
    obj->handle      = handle;
    obj->object_info = (uint32_t)item->handle;
    obj->rgb_coefficient = (int32_t)0xFFFFFFFF;   /* white, full blend     */
    obj->alpha_coefficient = 255;
    obj->visible     = true;
    obj->anim_seq    = 0;                         /* "Running"/first seq   */
    obj->anim_dir    = 0;
    obj->anim_frame  = 0;
    obj->forced_dir  = -1;
    obj->forced_frame = -1;
    obj->speed       = 0.0;
    obj->rng         = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)handle * 0xFF51AFD7ED558CCDULL);
    obj->rng ^= obj->rng >> 33;
    return obj;
}

void fxp_object_destroy(fxp_object_t *obj)
{
    for (int32_t i = 0; i < obj->value_count; i++)
        fxp_value_free(&obj->values[i]);
    free(obj->values);
    for (int32_t i = 0; i < obj->string_count; i++)
        free(obj->strings[i]);
    free(obj->strings);
    free(obj);
}

fxp_value_t *fxp_object_value(fxp_object_t *obj, int32_t index)
{
    if (index < 0)
        return NULL;
    grow_values(obj, index);
    return &obj->values[index];
}

const char *fxp_object_string(fxp_object_t *obj, int32_t index)
{
    if (index < 0 || index >= obj->string_count)
        return "";
    return obj->strings[index] ? obj->strings[index] : "";
}

void fxp_object_set_string(fxp_object_t *obj, int32_t index, const char *s)
{
    if (index < 0)
        return;
    if (index >= obj->string_count) {
        int32_t n = obj->string_count ? obj->string_count : 8;
        while (n <= index)
            n *= 2;
        obj->strings = fxp_realloc(obj->strings, (size_t)n * sizeof(char *));
        for (int32_t i = obj->string_count; i < n; i++)
            obj->strings[i] = NULL;
        obj->string_count = n;
    }
    free(obj->strings[index]);
    obj->strings[index] = fxp_strdup(s ? s : "");
}

int32_t fxp_object_current_image(const fxp_object_t *obj)
{
    if (obj->type != FXP_OT_ACTIVE || !obj->item)
        return -1;
    const fxp_item_t *item = obj->item;
    if (obj->anim_seq >= item->as.active.anim_count)
        return -1;
    const fxp_anim_t *anim = &item->as.active.anims[obj->anim_seq];
    int32_t dir = obj->anim_dir;
    if (obj->forced_dir >= 0)
        dir = obj->forced_dir;
    if (dir >= anim->dir_count)
        dir = 0;
    const fxp_anim_dir_t *d = &anim->dirs[dir];
    if (d->frame_count == 0)
        return -1;
    int32_t frame = obj->anim_frame;
    if (obj->forced_frame >= 0) {
        frame = obj->forced_frame;
        if (frame >= d->frame_count)
            frame = d->frame_count - 1;
    }
    return d->frames[frame];
}

bool fxp_object_update(fxp_object_t *obj, double dt)
{
    /* Animation advance — 100 units per frame at 60fps, ported from
     * NuclearRT Animations (speed * dt * 60 accumulating to 100). */
    if (obj->type != FXP_OT_ACTIVE || !obj->item || obj->anim_stopped)
        return false;
    if (obj->anim_seq >= obj->item->as.active.anim_count)
        return false;
    const fxp_anim_t *anim = &obj->item->as.active.anims[obj->anim_seq];
    int32_t dir = obj->anim_dir;
    if (obj->forced_dir >= 0)
        dir = obj->forced_dir;
    if (dir >= anim->dir_count)
        return false;
    const fxp_anim_dir_t *d = &anim->dirs[dir];
    if (d->frame_count <= 1 || obj->forced_frame >= 0)
        return false;

    bool over_edge = false;
    double speed = (d->min_speed != d->max_speed && obj->speed > 0)
                       ? (double)d->min_speed +
                         (d->max_speed - d->min_speed) * (obj->speed / 100.0)
                       : (double)d->max_speed;
    obj->anim_clock += speed * dt * 60.0;
    while (obj->anim_clock >= 100.0) {
        obj->anim_clock -= 100.0;
        obj->anim_frame++;
        if (obj->anim_frame >= d->frame_count) {
            obj->anim_over = true;
            over_edge = true;
            if (d->repeat)
                obj->anim_frame = d->back_to > 0 && d->back_to < d->frame_count
                                      ? d->back_to : 0;
            else
                obj->anim_frame = d->frame_count - 1;
        }
    }
    return over_edge;
}
