/*
 * Fusion X-Port — object.h
 * Runtime object instance.
 *
 * Mirrors NuclearRT's ObjectInstance (runtime/include/ObjectInstance.h):
 * an instance references its frame-item definition (object info) and owns
 * per-run mutable state (position, alterable values, animation cursor).
 */
#ifndef FXP_OBJECT_H
#define FXP_OBJECT_H

#include "fxp.h"
#include "value.h"
#include "mfa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxp_object {
    const fxp_item_t *item;          /* definition (not owned)           */
    uint32_t type;                    /* Fusion object type               */
    uint32_t handle;                  /* instance handle                  */
    uint32_t object_info;             /* item handle                      */
    uint32_t layer;
    int32_t  x, y;
    uint32_t angle;
    int32_t  rgb_coefficient;         /* legacy packed blend               */
    uint8_t  ink_effect;
    uint8_t  ink_effect_param;
    uint8_t  alpha_coefficient;       /* AlphaCoefficient action           */
    uint8_t  tint_r, tint_g, tint_b;  /* RGBCoefficient action (0x00bbggrr)*/
    bool     has_tint;

    /* animation state (ACTIVE) */
    int32_t  anim_seq;                /* sequence index into item anims   */
    int32_t  anim_dir;                /* direction 0..31                  */
    int32_t  anim_frame;              /* index into direction frames      */
    double   anim_clock;
    bool     anim_over;
    bool     anim_stopped;            /* Start/Stop animation action      */
    int32_t  forced_dir;              /* Force direction (-1 = none)      */
    int32_t  forced_frame;            /* Force frame (-1 = none)          */

    /* movement state — engine in movement.c */
    double   speed;                   /* Fusion "movement speed" value    */
    int32_t  direction;               /* 0..31, 0=right CCW (y-up)        */
    bool     movement_stopped;
    uint32_t movement_index;          /* selected entry in common.movements */
    bool     movement_ready;          /* engine state initialised         */
    double   real_speed;              /* ball/8dir live speed (px-scale)  */
    double   sub_x, sub_y;            /* sub-pixel accumulators           */
    double   stop_speed;              /* speed parked by Stop()           */
    bool     has_stop_speed;
    uint64_t rng;                     /* movement RNG (bouncing ball)     */

    /* "Is colliding with another object" (-14) edge state: bit i set when
     * the instance was overlapping some instance of the OI whose frame
     * item index is i during the last evaluation (item index < 64). */
    uint64_t collide_seen;

    bool     visible;
    bool     marked_for_delete;
    bool     created_this_tick;

    /* alterable values / strings — grow on demand */
    fxp_value_t *values;
    int32_t      value_count;
    char       **strings;
    int32_t      string_count;
} fxp_object_t;

fxp_object_t *fxp_object_create(const fxp_item_t *item, uint32_t handle);
void          fxp_object_destroy(fxp_object_t *obj);

/* Alterable accessors with Fusion grow-on-index semantics. */
fxp_value_t *fxp_object_value(fxp_object_t *obj, int32_t index);
const char  *fxp_object_string(fxp_object_t *obj, int32_t index);
void         fxp_object_set_string(fxp_object_t *obj, int32_t index, const char *s);

/* Advance animation/movement for one tick.  Returns true when the
 * animation just completed ("on animation over" edge). */
bool fxp_object_update(fxp_object_t *obj, double dt);

/* Current image handle for drawing (ACTIVE) or -1. */
int32_t fxp_object_current_image(const fxp_object_t *obj);

#ifdef __cplusplus
}
#endif
#endif /* FXP_OBJECT_H */
