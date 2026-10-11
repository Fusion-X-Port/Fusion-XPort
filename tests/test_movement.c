/*
 * Fusion X-Port — tests/test_movement.c
 * Unit tests for the built-in movement engines (source/movement.c):
 *
 *   - Bouncing Ball: constant-speed travel on the 32-direction dial
 *     (NuclearRT Cosinus32/Sinus32 scaling), straight flight through
 *     the frame borders (Fusion balls do NOT bounce there), bounce off
 *     backdrop OBSTACLES only (axis-separated, obstacle type 1), the
 *     Bounce action reflection, angle snapping and Stop/Start/Speed.
 *   - 8 Directions: control-driven acceleration/deceleration with the
 *     direction mask.
 *
 * The MFA/frames are built directly as structs (no file round-trip) so
 * every geometry assertion is exact.
 */
#include "fxp/mfa.h"
#include "fxp/mfa_format.h"
#include "fxp/frame.h"
#include "fxp/runtime.h"
#include "fxp/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok: %s\n", msg);                                      \
        } else {                                                            \
            printf("  FAIL: %s\n", msg);                                    \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

#define IMG_BALL   1
#define IMG_WALL   2

/* ---- in-memory MFA construction -------------------------------------- */

typedef struct world {
    fxp_mfa_t        mfa;
    fxp_image_t      images[2];
    uint8_t          ball_mask[2 * 16];
    uint8_t          wall_mask[2 * 64];
    fxp_anim_t       ball_anim;
    fxp_anim_dir_t   ball_dirs[32];
    int32_t          ball_frames[1];
    fxp_movement_t   ball_mv;
    fxp_movement_t   hero_mv;
    fxp_item_t       ball_item;
    fxp_item_t       wall_item;
    fxp_item_t       hero_item;
    fxp_item_t      *items[3];
    fxp_mfa_frame_t  def;
    fxp_mfa_frame_t *def_ptr;
    fxp_mfa_instance_t inst[3];
} world_t;

static void make_image(fxp_image_t *im, uint8_t *mask, int32_t handle,
                       int32_t w, int32_t h)
{
    memset(im, 0, sizeof(*im));
    im->handle = handle;
    im->width = (int16_t)w;
    im->height = (int16_t)h;
    im->col_mask = mask;
    im->col_mask_stride = (uint32_t)((w + 7) / 8);
    size_t rows = (size_t)h;
    memset(mask, 0, rows * im->col_mask_stride);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++)
            mask[(size_t)y * im->col_mask_stride + (size_t)(x >> 3)] |=
                (uint8_t)(0x80 >> (x & 7));
    }
}

/* dir_count=32 single-frame animation so every direction has an image */
static void make_ball_anim(world_t *w)
{
    w->ball_frames[0] = IMG_BALL;
    for (int32_t d = 0; d < 32; d++) {
        memset(&w->ball_dirs[d], 0, sizeof(fxp_anim_dir_t));
        w->ball_dirs[d].index = (int16_t)d;
        w->ball_dirs[d].frame_count = 1;
        w->ball_dirs[d].frames = w->ball_frames;
        w->ball_dirs[d].min_speed = 100;
        w->ball_dirs[d].max_speed = 100;
    }
    memset(&w->ball_anim, 0, sizeof(fxp_anim_t));
    w->ball_anim.dir_count = 32;
    w->ball_anim.dirs = w->ball_dirs;
}

static void make_ball_item(world_t *w, int32_t speed, int32_t angles,
                           int32_t decel, int32_t dir_mask, int moving)
{
    fxp_item_t *it = &w->ball_item;
    memset(it, 0, sizeof(*it));
    it->object_type = FXP_OT_ACTIVE;
    it->handle = 10;
    it->common.new_object_flags = 0x000C;   /* visible + box collisions  */
    it->common.movement_count = 1;
    it->common.movements = &w->ball_mv;
    it->as.active.anim_count = 1;
    it->as.active.anims = &w->ball_anim;

    memset(&w->ball_mv, 0, sizeof(fxp_movement_t));
    w->ball_mv.type = FXP_MOV_BOUNCING_BALL;
    w->ball_mv.moving_at_start = (uint8_t)moving;
    w->ball_mv.direction_at_start = dir_mask;
    w->ball_mv.mv_speed = speed;
    w->ball_mv.mv_angles = angles;
    w->ball_mv.mv_deceleration = decel;
}

static void make_hero_item(world_t *w)
{
    fxp_item_t *it = &w->hero_item;
    memset(it, 0, sizeof(*it));
    it->object_type = FXP_OT_ACTIVE;
    it->handle = 11;
    it->common.new_object_flags = 0x000C;
    it->common.movement_count = 1;
    it->common.movements = &w->hero_mv;
    it->as.active.anim_count = 1;
    it->as.active.anims = &w->ball_anim;    /* shared 32-dir single anim */

    memset(&w->hero_mv, 0, sizeof(fxp_movement_t));
    w->hero_mv.type = FXP_MOV_EIGHT_DIRECTIONS;
    w->hero_mv.moving_at_start = 1;
    w->hero_mv.mv_speed = 60;
    w->hero_mv.mv_acceleration = 50;
    w->hero_mv.mv_deceleration = 50;
    w->hero_mv.mv_directions = 0x11111111;  /* all 8                     */
}

static void make_wall_item(world_t *w)
{
    fxp_item_t *it = &w->wall_item;
    memset(it, 0, sizeof(*it));
    it->object_type = FXP_OT_BACKDROP;
    it->handle = 12;
    it->as.backdrop.image = IMG_WALL;
    it->as.backdrop.obstacle_type = 1;   /* Fusion: solid obstacle       */
}

static void make_frame(world_t *w, int32_t n_inst)
{
    fxp_mfa_frame_t *def = &w->def;
    memset(def, 0, sizeof(*def));
    def->handle = 0;
    def->size_x = 400;
    def->size_y = 240;
    def->items = w->items;
    def->item_count = 3;                  /* all items, any subset used */
    w->items[0] = &w->ball_item;
    w->items[1] = &w->hero_item;
    w->items[2] = &w->wall_item;
    def->instances = w->inst;
    def->instance_count = n_inst;
}

static fxp_frame_t *spawn(world_t *w, int32_t n_inst, int32_t ball_x,
                          int32_t ball_y, bool with_wall)
{
    memset(&w->inst, 0, sizeof(w->inst));
    w->inst[0].item_handle = 10;
    w->inst[0].x = ball_x;
    w->inst[0].y = ball_y;
    w->inst[0].handle = 1;
    int32_t n = 1;
    if (n_inst > 1 && !with_wall) {
        w->inst[1].item_handle = 11;
        w->inst[1].x = 40;
        w->inst[1].y = 40;
        w->inst[1].handle = 2;
        n = 2;
    }
    if (with_wall) {
        w->inst[n].item_handle = 12;
        w->inst[n].x = 200;               /* vertical wall 200..215     */
        w->inst[n].y = 0;
        w->inst[n].handle = 3;
        n++;
    }
    make_frame(w, n);
    fxp_frame_t *fr = fxp_frame_create(&w->mfa, &w->def);
    fxp_frame_spawn_instances(fr);
    return fr;
}

static void world_init(world_t *w)
{
    memset(w, 0, sizeof(*w));
    make_image(&w->images[0], w->ball_mask, IMG_BALL, 16, 16);
    make_image(&w->images[1], w->wall_mask, IMG_WALL, 16, 240);
    make_ball_anim(w);
    make_wall_item(w);
    w->mfa.images.count = 2;
    w->mfa.images.items = w->images;
    w->mfa.frame_count = 1;
    w->mfa.frames = &w->def_ptr;
    w->def_ptr = &w->def;
}

static void world_free(fxp_frame_t *fr)
{
    fxp_frame_destroy(fr);                /* objects + selectors + rt   */
}

static void tick(fxp_frame_t *fr, int32_t times)
{
    for (int32_t i = 0; i < times; i++) {
        fxp_frame_update(fr, 1.0 / 60.0);
        fxp_runtime_end_tick(fr->rt);
    }
}

static fxp_object_t *ball_of(fxp_frame_t *fr)
{
    return fxp_frame_find_by_info(fr, 10);
}

static fxp_object_t *hero_of(fxp_frame_t *fr)
{
    return fxp_frame_find_by_info(fr, 11);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Fusion X-Port: movement engines (bouncing ball, 8 dir) ==\n");

    /* ---- 1. ball flies straight through the frame border ----------- *
     * speed 64, direction 0: 256*64/2048 = 8 px per tick.  Fusion
     * bouncing balls do NOT bounce at the frame edges — after 60 ticks
     * the ball is far off the 400px frame, still heading right. */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        fxp_frame_t *fr = spawn(&w, 1, 0, 100, false);
        fxp_object_t *ball = ball_of(fr);
        CHECK(ball && ball->direction == 0 && ball->real_speed == 64.0,
              "ball spawns moving right at speed 64");
        tick(fr, 60);
        CHECK(ball->x == 480,
              "ball crosses the border without bouncing (8*60 = 480)");
        CHECK(ball->direction == 0,
              "frame border never flips the direction");
        CHECK(ball->y == 100, "horizontal travel leaves y alone");
        world_free(fr);
    }

    /* ---- 2. ball bounces off obstacle-flagged backdrop walls -------- *
     * Wall at x=200 (16 wide, full height, obstacle type 1).  Ball from
     * (100,100) moving right: last free x is 180 (right edge 196), tick
     * 11 hits, dir flips to 16, then 8 px/tick left.  30 ticks ->
     * 180 - 19*8 = 28. */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        fxp_frame_t *fr = spawn(&w, 1, 100, 100, true);   /* ball + wall  */
        fxp_object_t *ball = ball_of(fr);
        tick(fr, 30);
        CHECK(ball->direction == 16, "backdrop obstacle flips direction");
        CHECK(ball->x == 28, "ball retreats from the wall (x=28)");
        CHECK(ball->x + 16 <= 200, "ball never entered the wall");
        world_free(fr);
    }

    /* ---- 2b. decorative backdrops are NOT walls ---------------------- *
     * Same geometry as test 2, but the backdrop is obstacle type 0
     * (plain decoration): the ball passes straight through it. */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        w.wall_item.as.backdrop.obstacle_type = 0;
        fxp_frame_t *fr = spawn(&w, 1, 100, 100, true);   /* ball + deco  */
        fxp_object_t *ball = ball_of(fr);
        tick(fr, 30);
        CHECK(ball->direction == 0,
              "decorative backdrop never flips the direction");
        CHECK(ball->x == 100 + 8 * 30,
              "ball flies straight through the decoration");
        world_free(fr);
    }

    /* ---- 3. Bounce action reflection -------------------------------- *
     * Overlapping partner on the right: normal (partner -> ball) is
     * direction 16 (left); dir' = 2*16 + 16 - 0 = 48 & 31 = 16. */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        make_hero_item(&w);
        fxp_frame_t *fr = spawn(&w, 2, 100, 100, false);
        fxp_object_t *ball = ball_of(fr);
        fxp_object_t *hero = hero_of(fr);
        ball->direction = 0;
        hero->x = 104;                    /* overlapping the 16px ball  */
        hero->y = 100;
        fxp_object_movement_bounce(fr, ball);
        CHECK(ball->direction == 16,
              "Bounce off a partner on the right reverses to dir 16");
        hero->x = 40;
        hero->y = 40;
        ball->direction = 0;
        fxp_object_movement_bounce(fr, ball);
        CHECK(ball->direction == 16,
              "Bounce with no collision reverses (official rule)");
        world_free(fr);
    }

    /* ---- 4. Stop / Start park and restore the speed ----------------- */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        fxp_frame_t *fr = spawn(&w, 1, 0, 100, false);
        fxp_object_t *ball = ball_of(fr);
        fxp_object_movement_stop(ball);
        CHECK(ball->movement_stopped && ball->real_speed == 0.0 &&
                  ball->stop_speed == 64.0,
              "Stop parks the current speed");
        int32_t x0 = ball->x;
        tick(fr, 5);
        CHECK(ball->x == x0, "stopped ball does not move");
        fxp_object_movement_start(ball);
        CHECK(ball->real_speed == 64.0 && !ball->movement_stopped,
              "Start restores the parked speed");
        tick(fr, 3);
        CHECK(ball->x == x0 + 24, "moving again at 8 px/tick");
        fxp_object_movement_set_speed(ball, 32);
        CHECK(ball->real_speed == 32.0, "Set speed rescales the ball");
        tick(fr, 1);
        CHECK(ball->x == x0 + 24 + 4, "speed 32 -> 4 px/tick");
        world_free(fr);
    }

    /* ---- 5. angle snapping (Angles = 4 keeps 0/8/16/24) ------------- */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 4, 0, 1 << 5, 1);   /* initial dir 5      */
        fxp_frame_t *fr = spawn(&w, 1, 50, 50, false);
        fxp_object_t *ball = ball_of(fr);
        tick(fr, 1);
        CHECK(ball->direction == 8, "Angles=4 snaps dir 5 to 8 (up)");
        CHECK(ball->y < 50, "snapped ball travels upward");
        world_free(fr);
    }

    /* ---- 6. deceleration stops the ball ----------------------------- *
     * decel 50 -> accelerator 0x190 (400); speed 8 loses 400/256 = 1.5625
     * per tick (integer part: real_speed floors the travel), so the ball
     * halts within a handful of ticks. */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 16, 32, 50, 1 << 0, 1);
        fxp_frame_t *fr = spawn(&w, 1, 10, 100, false);
        fxp_object_t *ball = ball_of(fr);
        tick(fr, 40);
        CHECK(ball->real_speed == 0.0, "deceleration drains speed to 0");
        CHECK(ball->x < 10 + 16, "decelerated ball travels a short distance");
        int32_t stopped_x = ball->x;
        tick(fr, 10);
        CHECK(ball->x == stopped_x, "halted ball stays put");
        world_free(fr);
    }

    /* ---- 7. 8 directions: controls, acceleration, mask -------------- */
    {
        world_t w;
        world_init(&w);
        make_hero_item(&w);
        fxp_frame_t *fr = spawn(&w, 2, 10, 10, false);
        fxp_object_t *hero = hero_of(fr);
        hero->x = 10;
        hero->y = 10;
        CHECK(hero != NULL, "8-dir hero spawned");
        CHECK(hero->movement_stopped == false && hero->real_speed == 0.0,
              "8-dir starts idle");
        fr->rt->input.controls_down[0] = (uint16_t)(1u << FXP_CTRL_RIGHT);
        tick(fr, 6);
        CHECK(hero->direction == 0, "holding Right selects dir 0");
        CHECK(hero->x > 10 && hero->y == 10, "hero accelerates right");
        fr->rt->input.controls_down[0] =
            (uint16_t)((1u << FXP_CTRL_RIGHT) | (1u << FXP_CTRL_UP));
        tick(fr, 6);
        CHECK(hero->direction == 4, "Right+Up selects dir 4");
        CHECK(hero->y < 10, "diagonal climbs");
        fr->rt->input.controls_down[0] = 0;
        int32_t vx = hero->x;
        tick(fr, 3);
        CHECK(hero->x > vx, "released controls keep coasting (decel)");
        int32_t x_after = hero->x;
        tick(fr, 40);
        CHECK(hero->real_speed == 0.0 && hero->x > x_after &&
                  hero->x - x_after < 20,
              "deceleration settles within a few pixels");
        world_free(fr);
    }

    /* ---- 8. 8-direction mask blocks disallowed directions ----------- */
    {
        world_t w;
        world_init(&w);
        make_hero_item(&w);
        w.hero_mv.mv_directions = (1 << 0);      /* only Right allowed  */
        fxp_frame_t *fr = spawn(&w, 2, 10, 10, false);
        fxp_object_t *hero = hero_of(fr);
        hero->x = 10;
        hero->y = 10;
        fr->rt->input.controls_down[0] = (uint16_t)(1u << FXP_CTRL_UP);
        tick(fr, 5);
        CHECK(hero->y == 10 && hero->real_speed == 0.0,
              "direction mask blocks Up (no motion)");
        fr->rt->input.controls_down[0] = (uint16_t)(1u << FXP_CTRL_RIGHT);
        tick(fr, 5);
        CHECK(hero->x > 10, "allowed direction still moves");
        world_free(fr);
    }

    /* ---- 9. static movement stays put ------------------------------- */
    {
        world_t w;
        world_init(&w);
        make_ball_item(&w, 64, 32, 0, 1 << 0, 1);
        w.ball_item.common.movements->type = 0;   /* static             */
        fxp_frame_t *fr = spawn(&w, 1, 30, 30, false);
        fxp_object_t *ball = ball_of(fr);
        tick(fr, 10);
        CHECK(ball->x == 30 && ball->y == 30,
              "static movement never moves");
        world_free(fr);
    }

    if (g_failures == 0) {
        printf("== ALL MOVEMENT TESTS PASSED ==\n");
        return 0;
    }
    printf("== %d TEST(S) FAILED ==\n", g_failures);
    return 1;
}
