/*
 * Fusion X-Port — movement.c
 * Built-in movement engines: Bouncing Ball and 8 Directions.
 *
 * Fixed-point tables, speed scaling and the accel/decel model are ported
 * from NuclearRT's Movement.h / BouncingBallMovement.cpp /
 * EightDirectionsMovement.cpp (direction 0 = right, counter-clockwise,
 * 32 steps; Sinus32 is pre-negated for screen coordinates so that
 * delta_y = SIN[dir] * speed moves direction 8 upward).
 *
 * Bouncing-ball bounce semantics (real Fusion behaviour):
 *  - The ball does NOT bounce off the frame borders — it flies straight
 *    out of the frame and keeps going (games handle that with
 *    "leaving the frame" events or obstacle backdrops placed at the
 *    edges).  NuclearRT agrees: its ball never checks the frame size.
 *  - The ball DOES bounce off backdrop/quick-backdrop objects flagged
 *    as obstacles (Fusion obstacle type 1; decorative backdrops and
 *    ladders are ignored).  The reflection is an axis-separated sweep
 *    with an "already inside" escape so balls never stick.
 *  - Plus the editor's angle-snap (Angles = 4/8/16/32) and the Bounce
 *    action (reflect around the normal of whatever it is touching;
 *    outside a collision it reverses, per the official docs).
 */
#include "fxp/runtime.h"
#include "fxp/frame.h"
#include "fxp/mfa_format.h"

#include <math.h>
#include <stdlib.h>

/* ================================================== NuclearRT tables == */

static const int32_t COS32[32] = {
    256, 251, 236, 212, 181, 142, 97, 49,
    0, -49, -97, -142, -181, -212, -236, -251,
    -256, -251, -236, -212, -181, -142, -97, -49,
    0, 49, 97, 142, 181, 212, 236, 251
};

static const int32_t SIN32[32] = {
    0, -49, -97, -142, -181, -212, -236, -251,
    -256, -251, -236, -212, -181, -142, -97, -49,
    0, 49, 97, 142, 181, 212, 236, 251,
    256, 251, 236, 212, 181, 142, 97, 49
};

static const int32_t ACCELERATORS[101] = {
    0x0002, 0x0003, 0x0004, 0x0006, 0x0008, 0x000a, 0x000c, 0x0010, 0x0014, 0x0018,
    0x0030, 0x0038, 0x0040, 0x0048, 0x0050, 0x0058, 0x0060, 0x0068, 0x0070, 0x0078,
    0x0090, 0x00A0, 0x00B0, 0x00c0, 0x00d0, 0x00e0, 0x00f0, 0x0100, 0x0110, 0x0120,
    0x0140, 0x0150, 0x0160, 0x0170, 0x0180, 0x0190, 0x01a0, 0x01b0, 0x01c0, 0x01e0,
    0x0200, 0x0220, 0x0230, 0x0250, 0x0270, 0x0280, 0x02a0, 0x02b0, 0x02d0, 0x02e0,
    0x0300, 0x0310, 0x0330, 0x0350, 0x0360, 0x0380, 0x03a0, 0x03b0, 0x03d0, 0x03e0,
    0x0400, 0x0460, 0x04c0, 0x0520, 0x05a0, 0x0600, 0x0660, 0x06c0, 0x0720, 0x07a0,
    0x0800, 0x08c0, 0x0980, 0x0a80, 0x0b40, 0x0c00, 0x0cc0, 0x0d80, 0x0e80, 0x0f40,
    0x1000, 0x1990, 0x1332, 0x1460, 0x1664, 0x1800, 0x1999, 0x1b32, 0x1cc6, 0x1e64,
    0x2000, 0x266c, 0x2d98, 0x3404, 0x3a70, 0x40dc, 0x4748, 0x4db4, 0x5400, 0x6400,
    0x6400
};

static int32_t accel_value(int32_t a)
{
    if (a <= 0)
        return 0;
    if (a > 100)
        return a << 8;
    return ACCELERATORS[a];
}

/* Deterministic per-instance RNG (xorshift64). */
static uint64_t mov_rng(fxp_object_t *obj)
{
    uint64_t s = obj->rng;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    obj->rng = s;
    return s;
}

static int32_t mov_rand_range(fxp_object_t *obj, int32_t lo, int32_t hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int32_t)(mov_rng(obj) % (uint64_t)(hi - lo + 1));
}

/* ==================================================== geometry helpers == */

static const fxp_image_t *mvt_image(const fxp_frame_t *frame, int32_t handle)
{
    if (!frame->mfa || handle < 0)
        return NULL;
    for (int32_t i = 0; i < frame->mfa->images.count; i++)
        if (frame->mfa->images.items[i].handle == handle)
            return &frame->mfa->images.items[i];
    return NULL;
}

static bool mvt_mask_solid(const fxp_image_t *im, int32_t lx, int32_t ly)
{
    if (!im || !im->col_mask || lx < 0 || ly < 0 ||
        lx >= im->width || ly >= im->height)
        return false;
    return (im->col_mask[(size_t)ly * im->col_mask_stride +
                         (size_t)(lx >> 3)] & (0x80 >> (lx & 7))) != 0;
}

/* Fusion obstacle types (backdrop/quick-backdrop loaders): 0 = plain
 * decoration, 1 = obstacle, 2 = ladder.  Only real obstacles stop the
 * ball. */
static bool mvt_is_obstacle(const fxp_object_t *b)
{
    if (b->type == FXP_OT_BACKDROP)
        return b->item->as.backdrop.obstacle_type == 1;
    if (b->type == FXP_OT_QUICK_BACKDROP)
        return b->item->as.quick_backdrop.obstacle == 1;
    return false;
}

/* Any backdrop instance (pixel mask) or quick backdrop (box) flagged as
 * an obstacle at `obj`'s current position — the ball's wall test. */
static bool mvt_hits_backdrop(const fxp_frame_t *frame, const fxp_object_t *obj)
{
    int32_t ax = obj->x, ay = obj->y;
    int32_t aw = fxp_object_image_width(frame, obj);
    int32_t ah = fxp_object_image_height(frame, obj);
    const fxp_image_t *ia = (obj->type == FXP_OT_ACTIVE)
        ? mvt_image(frame, fxp_object_current_image(obj)) : NULL;

    for (int32_t i = 0; i < frame->instance_count; i++) {
        const fxp_object_t *b = frame->instances[i];
        if (b == obj || b->marked_for_delete)
            continue;
        if (!mvt_is_obstacle(b))
            continue;
        if (b->type == FXP_OT_BACKDROP) {
            int32_t bx = b->x, by = b->y;
            int32_t bw = fxp_object_image_width(frame, b);
            int32_t bh = fxp_object_image_height(frame, b);
            int32_t x0 = ax > bx ? ax : bx;
            int32_t y0 = ay > by ? ay : by;
            int32_t x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
            int32_t y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
            if (x0 >= x1 || y0 >= y1)
                continue;
            const fxp_image_t *ib = mvt_image(frame,
                                              (int32_t)b->item->as.backdrop.image);
            if (!ib || !ib->col_mask || !ia || !ia->col_mask)
                return true;              /* box-vs-box / no mask      */
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t x = x0; x < x1; x++) {
                    if (mvt_mask_solid(ia, x - ax, y - ay) &&
                        mvt_mask_solid(ib, x - bx, y - by))
                        return true;
                }
            }
            continue;
        }
        if (b->type == FXP_OT_QUICK_BACKDROP) {
            int32_t bw = b->item->as.quick_backdrop.w;
            int32_t bh = b->item->as.quick_backdrop.h;
            if (ax < b->x + bw && b->x < ax + aw &&
                ay < b->y + bh && b->y < ay + ah)
                return true;
        }
    }
    return false;
}

/* ================================================== movement plumbing == */

static const fxp_movement_t *obj_movement(const fxp_object_t *obj)
{
    if (!obj->item || obj->item->common.movement_count <= 0)
        return NULL;
    uint32_t idx = obj->movement_index;
    if (idx >= (uint32_t)obj->item->common.movement_count)
        idx = 0;
    return &obj->item->common.movements[idx];
}

/* Direction dial: 0 = right, CCW, 32 steps.  Syncs the animation. */
static void mvt_set_direction(fxp_object_t *obj, int32_t dir)
{
    dir &= 31;
    if (obj->direction == dir)
        return;
    obj->direction = dir;
    if (obj->forced_dir < 0) {
        obj->anim_dir = dir;
        obj->anim_frame = 0;
        obj->anim_clock = 0.0;
    }
}

static bool mvt_dir_valid(int32_t dir, int32_t mask)
{
    return dir >= 0 && dir < 32 && ((mask >> dir) & 1) != 0;
}

/* Bouncing Ball "Angles" snap: only directions that are multiples of
 * 32/angles stay; anything else snaps to the nearest allowed one. */
static int32_t mvt_snap_angles(const fxp_movement_t *mv, int32_t dir)
{
    int32_t angles = mv ? mv->mv_angles : 0;
    if (angles != 4 && angles != 8 && angles != 16)
        return dir & 31;                  /* 32 (or unset) = free       */
    int32_t step = 32 / angles;
    int32_t snapped = ((dir + step / 2) / step) * step;
    return snapped & 31;
}

static void mvt_pick_start_direction(fxp_object_t *obj,
                                     const fxp_movement_t *mv)
{
    int32_t mask = mv->direction_at_start;
    int32_t dirs[32], n = 0;
    for (int32_t d = 0; d < 32; d++)
        if ((mask >> d) & 1)
            dirs[n++] = d;
    if (n > 0)
        mvt_set_direction(obj, dirs[mov_rand_range(obj, 0, n - 1)]);
    else if (mv->type == FXP_MOV_EIGHT_DIRECTIONS)
        mvt_set_direction(obj, 0);        /* NuclearRT: default right   */
}

/* OnEnabled — initial speed/direction/stopped state. */
void fxp_object_init_movement(fxp_frame_t *frame, fxp_object_t *obj)
{
    (void)frame;
    obj->movement_ready = true;
    obj->sub_x = obj->sub_y = 0.0;
    obj->has_stop_speed = false;
    obj->stop_speed = 0.0;

    const fxp_movement_t *mv = obj_movement(obj);
    if (!mv || mv->type == FXP_MOV_MOUSE) {
        obj->movement_stopped = true;
        return;
    }
    if (mv->type != FXP_MOV_BOUNCING_BALL &&
        mv->type != FXP_MOV_EIGHT_DIRECTIONS &&
        mv->type != FXP_MOV_RACE) {
        /* path/platform: not implemented yet — behave as parked */
        obj->movement_stopped = true;
        return;
    }

    obj->movement_stopped = false;
    mvt_pick_start_direction(obj, mv);

    if (mv->type == FXP_MOV_BOUNCING_BALL) {
        double sp = (double)(mv->mv_speed < 0 ? 0 :
                             (mv->mv_speed > 250 ? 250 : mv->mv_speed));
        if (mv->mv_randomizer > 0)
            sp += (double)mov_rand_range(obj, -mv->mv_randomizer,
                                         mv->mv_randomizer);
        if (sp < 0)
            sp = 0;
        if (sp > 250)
            sp = 250;
        obj->real_speed = sp;
        obj->speed = sp;
        if (!mv->moving_at_start) {
            obj->stop_speed = sp;         /* NuclearRT Stop()           */
            obj->has_stop_speed = true;
            obj->real_speed = 0.0;
            obj->speed = 0.0;
            obj->movement_stopped = true;
        }
    } else {
        obj->real_speed = 0.0;            /* 8-dir accelerates on input */
        obj->speed = 0.0;
    }
}

void fxp_object_movement_stop(fxp_object_t *obj)
{
    if (!obj->has_stop_speed) {
        obj->stop_speed = obj->real_speed;
        obj->has_stop_speed = true;
    }
    obj->real_speed = 0.0;
    obj->speed = 0.0;
    obj->movement_stopped = true;
}

void fxp_object_movement_start(fxp_object_t *obj)
{
    obj->movement_stopped = false;
    if (obj->has_stop_speed) {
        obj->real_speed = obj->stop_speed;
        obj->speed = obj->stop_speed;
        obj->has_stop_speed = false;
        obj->stop_speed = 0.0;
    }
}

void fxp_object_movement_set_speed(fxp_object_t *obj, int32_t speed)
{
    const fxp_movement_t *mv = obj_movement(obj);
    bool ball = mv && mv->type == FXP_MOV_BOUNCING_BALL;
    int32_t clamped = speed < 0 ? 0 : (speed > 250 ? 250 : speed);
    if (ball) {
        /* NuclearRT SetCurrentSpeed: clears the parked speed, resumes */
        obj->real_speed = (double)clamped;
        obj->has_stop_speed = false;
        obj->stop_speed = 0.0;
        obj->movement_stopped = false;
    }
    obj->speed = (double)(ball ? clamped : speed);
}

void fxp_object_movement_select(fxp_frame_t *frame, fxp_object_t *obj,
                                int32_t index)
{
    if (!obj->item || obj->item->common.movement_count <= 0)
        return;
    if (index < 0)
        index = 0;
    if (index >= obj->item->common.movement_count)
        index = obj->item->common.movement_count - 1;
    obj->movement_index = (uint32_t)index;
    fxp_object_init_movement(frame, obj);
}

/* ==================================================== bouncing ball == */

static void ball_update(fxp_frame_t *frame, fxp_object_t *obj,
                        const fxp_movement_t *mv, double dt)
{
    const double base = dt * 60.0;

    if (!obj->movement_stopped && mv->mv_deceleration > 0 &&
        obj->real_speed > 0.0) {
        obj->real_speed -= (double)accel_value(mv->mv_deceleration) *
                           base / 256.0;
        if (obj->real_speed < 0.0)
            obj->real_speed = 0.0;
        obj->speed = obj->real_speed;
    }

    const double sp = (double)(int32_t)obj->real_speed;
    const double dx = (double)COS32[obj->direction & 31] * sp * base / 2048.0;
    const double dy = (double)SIN32[obj->direction & 31] * sp * base / 2048.0;

    double nx = (double)obj->x + obj->sub_x + dx;
    double ny = (double)obj->y + obj->sub_y + dy;

    /* NOTE: no frame-border handling on purpose — Fusion's bouncing
     * ball never bounces at the frame edges, it just keeps flying (see
     * the file header).  Games place obstacle backdrops at the edges or
     * use "leaving the frame" events when they want walls. */

    /* ---- backdrop obstacles (axis-separated sweep) ------------------ */
    bool inside = mvt_hits_backdrop(frame, obj);   /* escape: don't stick */
    int32_t old_x = obj->x;
    int32_t new_x = (int32_t)nx;
    if (!inside && new_x != old_x) {
        obj->x = new_x;
        if (mvt_hits_backdrop(frame, obj)) {
            obj->x = old_x;
            obj->sub_x = 0.0;
            nx = (double)old_x;
            mvt_set_direction(obj, 16 - obj->direction);
        }
    }
    int32_t old_y = obj->y;
    int32_t new_y = (int32_t)ny;
    if (!inside && new_y != old_y) {
        obj->y = new_y;
        if (mvt_hits_backdrop(frame, obj)) {
            obj->y = old_y;
            obj->sub_y = 0.0;
            ny = (double)old_y;
            mvt_set_direction(obj, 32 - obj->direction);
        }
    }

    mvt_set_direction(obj, mvt_snap_angles(mv, obj->direction));

    obj->sub_x = nx - (double)(int32_t)nx;
    obj->sub_y = ny - (double)(int32_t)ny;
}

/* ==================================================== 8 directions == */

static void eight_dir_update(fxp_frame_t *frame, fxp_object_t *obj,
                             const fxp_movement_t *mv, double dt)
{
    fxp_runtime_t *rt = frame->rt;
    const uint16_t down = rt ? rt->input.controls_down[0] : 0;
    /* NuclearRT scales dt by 10 for both accel and position */
    const double sdt = dt * 10.0;

    int32_t wish_x = 0, wish_y = 0;
    if (down & (1u << FXP_CTRL_UP))    wish_y -= 1;
    if (down & (1u << FXP_CTRL_DOWN))  wish_y += 1;
    if (down & (1u << FXP_CTRL_LEFT))  wish_x -= 1;
    if (down & (1u << FXP_CTRL_RIGHT)) wish_x += 1;

    int32_t mask = mv->mv_directions ? mv->mv_directions : 0x11111111;
    int32_t dir = -1;
    if (wish_x == 0 && wish_y < 0 && mvt_dir_valid(8, mask))
        dir = 8;
    else if (wish_x > 0 && wish_y < 0 && mvt_dir_valid(4, mask))
        dir = 4;
    else if (wish_x > 0 && wish_y == 0 && mvt_dir_valid(0, mask))
        dir = 0;
    else if (wish_x > 0 && wish_y > 0 && mvt_dir_valid(28, mask))
        dir = 28;
    else if (wish_x == 0 && wish_y > 0 && mvt_dir_valid(24, mask))
        dir = 24;
    else if (wish_x < 0 && wish_y > 0 && mvt_dir_valid(20, mask))
        dir = 20;
    else if (wish_x < 0 && wish_y == 0 && mvt_dir_valid(16, mask))
        dir = 16;
    else if (wish_x < 0 && wish_y < 0 && mvt_dir_valid(12, mask))
        dir = 12;

    if (dir >= 0)
        mvt_set_direction(obj, dir);

    if (dir >= 0) {
        obj->real_speed += (double)mv->mv_acceleration * sdt;
        double max_sp = (double)mv->mv_speed;
        if (obj->real_speed > max_sp)
            obj->real_speed = max_sp;
    } else {
        obj->real_speed -= (double)mv->mv_deceleration * sdt;
        if (obj->real_speed < 0.0)
            obj->real_speed = 0.0;
    }
    obj->speed = obj->real_speed;

    if (obj->real_speed <= 0.0)
        return;

    double ang = (double)(obj->direction & 31) * 3.14159265358979 / 16.0;
    double dx = obj->real_speed * sdt * cos(ang);
    double dy = -obj->real_speed * sdt * sin(ang);

    obj->x += (int32_t)(dx >= 0 ? dx + 0.5 : dx - 0.5);
    obj->y += (int32_t)(dy >= 0 ? dy + 0.5 : dy - 0.5);
}

/* ======================================================== public API == */

/* Per-tick movement advance (NuclearRT: before the event pipeline). */
void fxp_object_movement_update(fxp_frame_t *frame, fxp_object_t *obj,
                                double dt)
{
    if (obj->marked_for_delete)
        return;
    const fxp_movement_t *mv = obj_movement(obj);
    if (!mv)
        return;
    if (!obj->movement_ready)
        fxp_object_init_movement(frame, obj);

    switch (mv->type) {
    case FXP_MOV_BOUNCING_BALL:
        ball_update(frame, obj, mv, dt);
        break;
    case FXP_MOV_EIGHT_DIRECTIONS:
        eight_dir_update(frame, obj, mv, dt);
        break;
    default:
        break;                            /* static / unimplemented     */
    }
}

/*
 * The Bounce action.  With a colliding partner: reflect the movement
 * direction around the normal of the vector partner -> object
 * (dir' = 2*normal + 16 - dir, mod 32).  Without one the official
 * docs say the object "simply reverses its direction".
 * An 8-direction object with bounce factor 0 stops instead.
 */
void fxp_object_movement_bounce(fxp_frame_t *frame, fxp_object_t *obj)
{
    fxp_object_t *other = NULL;
    for (int32_t i = 0; i < frame->instance_count && !other; i++) {
        fxp_object_t *o = frame->instances[i];
        if (o == obj || o->marked_for_delete)
            continue;
        if (fxp_collision_objects(frame, obj, o))
            other = o;
    }

    if (other) {
        double dx = (double)(obj->x - other->x);
        double dy = (double)(obj->y - other->y);
        double ang = atan2(-dy, dx);      /* y-up convention            */
        if (ang < 0)
            ang += 2.0 * 3.14159265358979;
        int32_t normal = (int32_t)(ang / (2.0 * 3.14159265358979) * 32.0 +
                                   0.5) & 31;
        mvt_set_direction(obj, 2 * normal + 16 - obj->direction);
    } else {
        mvt_set_direction(obj, obj->direction + 16);
    }

    const fxp_movement_t *mv = obj_movement(obj);
    if (mv && mv->type == FXP_MOV_EIGHT_DIRECTIONS &&
        mv->mv_bounce_factor == 0)
        fxp_object_movement_stop(obj);
}
