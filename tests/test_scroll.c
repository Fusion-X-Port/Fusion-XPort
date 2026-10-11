/*
 * Fusion X-Port — tests/test_scroll.c
 * Camera scroll tests (source/frame.c scroll API + the Fusion
 * "center display" app actions 7/8/9 in source/event_runtime.c):
 *
 *   1. NuclearRT SetScroll contract: the argument is the desired view
 *      CENTER; scroll = clamp(center - screen/2, 0, frame - screen).
 *   2. Frames smaller than the viewport never scroll (clamp to 0).
 *   3. Center-X / center-Y variants clamp independently.
 *   4. Layer coefficient compensation for coefficient > 1 layers.
 *   5. fxp_frame_layer_coeffs defaults (unknown layer / coef 0 -> 1.0).
 *   6. Event-driven: a zero-condition group running app action 7 with
 *      an absolute Position param, action 7 relative to a spawned
 *      object, and actions 8/9 with expression params.
 *   7. XLeftEdge/XRightEdge/YTopEdge/YBottomEdge expressions follow the
 *      camera.
 *
 * The MFA/frames/events are built directly as structs (no file
 * round-trip) so every assertion is exact.
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

#define IMG_DOT 1

#define OI_HERO 10

/* ---- in-memory MFA ---------------------------------------------------- */

typedef struct world {
    fxp_mfa_t        mfa;
    fxp_image_t      images[1];
    uint8_t          dot_mask[2 * 2];
    fxp_anim_t       anim;
    fxp_anim_dir_t   dirs[1];
    int32_t          dir_frames[1];
    fxp_item_t       hero_item;
    fxp_item_t      *items[1];
    fxp_mfa_layer_t  layers[2];
    fxp_mfa_instance_t inst[1];
    fxp_mfa_frame_t  def;
    fxp_mfa_frame_t *def_ptr;
    /* events */
    fxp_action_t     actions[2];
    fxp_param_t      params[2];
    uint8_t          pos_raw[12];
    fxp_expression_t expr[2];
    fxp_expr_item_t  expr_items[2][2];
    fxp_event_group_t group;
    fxp_events_t     events;
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
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < w; x++)
            mask[(size_t)y * im->col_mask_stride + (size_t)(x >> 3)] |=
                (uint8_t)(0x80 >> (x & 7));
}

static void world_init(world_t *w, int32_t frame_w, int32_t frame_h)
{
    memset(w, 0, sizeof(*w));
    make_image(&w->images[0], w->dot_mask, IMG_DOT, 2, 2);

    w->dir_frames[0] = IMG_DOT;
    w->dirs[0].index = 0;
    w->dirs[0].frame_count = 1;
    w->dirs[0].frames = w->dir_frames;
    w->anim.dir_count = 1;
    w->anim.dirs = w->dirs;

    w->hero_item.object_type = FXP_OT_ACTIVE;
    w->hero_item.handle = OI_HERO;
    w->hero_item.common.new_object_flags = 0x000C;   /* visible + box  */
    w->hero_item.common.movement_count = 0;
    w->hero_item.as.active.anim_count = 1;
    w->hero_item.as.active.anims = &w->anim;
    w->items[0] = &w->hero_item;

    /* two layers: plain (coef 1) + fast-x parallax (coef 2) */
    w->layers[0].name = fxp_strdup("plain");
    w->layers[0].x_coefficient = 1.0f;
    w->layers[0].y_coefficient = 1.0f;
    w->layers[1].name = fxp_strdup("fast");
    w->layers[1].x_coefficient = 2.0f;
    w->layers[1].y_coefficient = 1.0f;

    w->def.handle = 0;
    w->def.size_x = frame_w;
    w->def.size_y = frame_h;
    w->def.layers = w->layers;
    w->def.layer_count = 2;
    w->def.items = w->items;
    w->def.item_count = 1;

    w->mfa.images.count = 1;
    w->mfa.images.items = w->images;
    w->mfa.frame_count = 1;
    w->mfa.frames = &w->def_ptr;
    w->def_ptr = &w->def;
}

/* default frame: one static hero at (250, 200) on layer 0 */
static fxp_frame_t *spawn(world_t *w)
{
    w->inst[0].item_handle = OI_HERO;
    w->inst[0].x = 250;
    w->inst[0].y = 200;
    w->inst[0].handle = 1;
    w->inst[0].layer = 0;
    w->def.instances = w->inst;
    w->def.instance_count = 1;
    fxp_frame_t *fr = fxp_frame_create(&w->mfa, &w->def);
    fxp_frame_spawn_instances(fr);
    return fr;
}

/* empty frame (no instances) */
static fxp_frame_t *spawn_empty(world_t *w)
{
    w->def.instances = NULL;
    w->def.instance_count = 0;
    fxp_frame_t *fr = fxp_frame_create(&w->mfa, &w->def);
    fxp_frame_spawn_instances(fr);
    return fr;
}

/* ---- in-memory event building ----------------------------------------- */

/* App action with a Position parameter (code 16): payload after the
 * [size i16][code i16] header is { u16 parent, u16 pad, i16 x, i16 y }. */
static void make_pos_param(fxp_param_t *p, uint8_t *raw, int32_t parent,
                           int32_t x, int32_t y)
{
    memset(p, 0, sizeof(*p));
    p->code = FXP_PARAM_POSITION;
    raw[0] = 8;  raw[1] = 0;              /* payload size                */
    raw[2] = (uint8_t)(FXP_PARAM_POSITION & 0xFF);
    raw[3] = 0;
    raw[4] = (uint8_t)(parent & 0xFF);
    raw[5] = (uint8_t)((parent >> 8) & 0xFF);
    raw[6] = 0;  raw[7] = 0;
    raw[8] = (uint8_t)(x & 0xFF);
    raw[9] = (uint8_t)((x >> 8) & 0xFF);
    raw[10] = (uint8_t)(y & 0xFF);
    raw[11] = (uint8_t)((y >> 8) & 0xFF);
    p->raw = raw;
    p->raw_size = 12;
}

/* Expression parameter holding one system integer literal. */
static void make_expr_param(fxp_param_t *p, fxp_expression_t *e,
                            fxp_expr_item_t *items, int32_t value)
{
    memset(items, 0, sizeof(fxp_expr_item_t) * 2);
    items[0].object_type = -1;
    items[0].num = FXP_EXPR_LONG;
    items[0].int_val = value;
    /* items[1] stays {0, 0} — the terminator */
    memset(e, 0, sizeof(*e));
    e->comparison = 0;
    e->count = 2;
    e->items = items;
    memset(p, 0, sizeof(*p));
    p->code = FXP_PARAM_EXPRESSION;
    p->has_expression = true;
    p->expression = *e;
}

/* Zero-condition group (runs every normal-pass tick) with one action. */
static void make_group(world_t *w, int16_t type, int16_t num,
                       fxp_param_t *params, int32_t param_count)
{
    memset(&w->actions, 0, sizeof(w->actions));
    w->actions[0].object_type = type;
    w->actions[0].num = num;
    w->actions[0].param_count = param_count;
    w->actions[0].params = params;

    memset(&w->group, 0, sizeof(w->group));
    w->group.condition_count = 0;
    w->group.action_count = 1;
    w->group.actions = w->actions;

    memset(&w->events, 0, sizeof(w->events));
    w->events.group_count = 1;
    w->events.groups = &w->group;
    w->def.events = w->events;
}

static void tick(fxp_frame_t *fr, int32_t times)
{
    for (int32_t i = 0; i < times; i++) {
        fxp_frame_update(fr, 1.0 / 60.0);
        fxp_runtime_end_tick(fr->rt);
    }
}

static fxp_object_t *hero_of(fxp_frame_t *fr)
{
    return fxp_frame_find_by_info(fr, OI_HERO);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Fusion X-Port: camera scroll ==\n");

    /* ---- 1. SetScroll contract (NuclearRT Frame::SetScroll) --------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        fxp_frame_t *fr = spawn_empty(&w);
        CHECK(fr->scroll_x == 0.0 && fr->scroll_y == 0.0,
              "camera starts at the frame origin");
        fxp_frame_set_scroll(fr, 400, 240, -1);
        CHECK(fr->scroll_x == 200.0 && fr->scroll_y == 120.0,
              "center (400,240) puts the view at (200,120)");
        fxp_frame_set_scroll(fr, 10000, -500, -1);
        CHECK(fr->scroll_x == 400.0 && fr->scroll_y == 0.0,
              "scroll clamps to [0, frame - screen] = [0,400]x[0,240]");
        fxp_frame_set_scroll(fr, 0, 0, -1);
        CHECK(fr->scroll_x == 0.0 && fr->scroll_y == 0.0,
              "center at the origin clamps to no scroll");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 2. frames smaller than the viewport never scroll ----------- */
    {
        world_t w;
        world_init(&w, 200, 100);
        fxp_frame_t *fr = spawn_empty(&w);
        fxp_frame_set_scroll(fr, 5000, 5000, -1);
        CHECK(fr->scroll_x == 0.0 && fr->scroll_y == 0.0,
              "200x100 frame stays glued to (0,0)");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 3. center X / center Y clamp independently ----------------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        fxp_frame_t *fr = spawn_empty(&w);
        fxp_frame_set_scroll_x(fr, 600);          /* 600-200 = 400 -> max */
        fxp_frame_set_scroll_y(fr, 120);          /* 120-120 = 0          */
        CHECK(fr->scroll_x == 400.0 && fr->scroll_y == 0.0,
              "SetScrollX/Y clamp independently");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 4. layer coefficient compensation (NuclearRT rule) --------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        fxp_frame_t *fr = spawn_empty(&w);
        fxp_frame_set_scroll(fr, 400, 240, 1);    /* layer 1: x coef 2    */
        CHECK(fr->scroll_x == 100.0 && fr->scroll_y == 120.0,
              "coefficient-2 layer divides the x delta by 2");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 5. layer coefficient accessor ------------------------------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        fxp_frame_t *fr = spawn_empty(&w);
        float lx = 0.0f, ly = 0.0f;
        fxp_frame_layer_coeffs(fr, 1, &lx, &ly);
        CHECK(lx == 2.0f && ly == 1.0f, "layer 1 reports its coefficients");
        fxp_frame_layer_coeffs(fr, 0, &lx, &ly);
        CHECK(lx == 1.0f && ly == 1.0f, "layer 0 scrolls with the world");
        fxp_frame_layer_coeffs(fr, 99, &lx, &ly);
        CHECK(lx == 1.0f && ly == 1.0f,
              "unknown layer falls back to plain scrolling");
        w.layers[0].x_coefficient = 0.0f;         /* unset in old files  */
        fxp_frame_layer_coeffs(fr, 0, &lx, &ly);
        CHECK(lx == 1.0f, "coef 0 is tolerated as a plain layer");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 6. app action 7: Center display at absolute position -------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        make_pos_param(&w.params[0], w.pos_raw, 0xFFFF, 300, 240);
        make_group(&w, FXP_OT_GAME, 7, &w.params[0], 1);
        fxp_frame_t *fr = spawn_empty(&w);
        tick(fr, 2);
        CHECK(fr->scroll_x == 100.0 && fr->scroll_y == 120.0,
              "action 7 (absolute 300,240) scrolls to (100,120)");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 7. app action 7: Center display at an object ---------------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        make_pos_param(&w.params[0], w.pos_raw, OI_HERO, 0, 0);
        make_group(&w, FXP_OT_GAME, 7, &w.params[0], 1);
        fxp_frame_t *fr = spawn(&w);
        fxp_object_t *hero = hero_of(fr);
        CHECK(hero != NULL, "hero spawned");
        tick(fr, 2);
        CHECK(fr->scroll_x == 50.0 && fr->scroll_y == 80.0,
              "action 7 (object-relative) centers on the hero");
        /* moving the hero moves the camera on the next tick */
        hero->x = 600;
        hero->y = 400;
        tick(fr, 2);
        CHECK(fr->scroll_x == 400.0 && fr->scroll_y == 240.0,
              "camera follows the hero (clamped to the frame edge)");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 8. app actions 8/9: Center display at X/Y (expressions) ----- */
    {
        world_t w;
        world_init(&w, 800, 480);
        make_expr_param(&w.params[0], &w.expr[0], w.expr_items[0], 500);
        make_group(&w, FXP_OT_GAME, 8, &w.params[0], 1);
        fxp_frame_t *fr = spawn_empty(&w);
        tick(fr, 2);
        CHECK(fr->scroll_x == 300.0 && fr->scroll_y == 0.0,
              "action 8 (X = 500) scrolls x to 300, y untouched");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }
    {
        world_t w;
        world_init(&w, 800, 480);
        make_expr_param(&w.params[0], &w.expr[0], w.expr_items[0], 360);
        make_group(&w, FXP_OT_GAME, 9, &w.params[0], 1);
        fxp_frame_t *fr = spawn_empty(&w);
        tick(fr, 2);
        CHECK(fr->scroll_x == 0.0 && fr->scroll_y == 240.0,
              "action 9 (Y = 360) scrolls y to 240 (clamped), x untouched");
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    /* ---- 9. scroll-edge expressions follow the camera ---------------- */
    {
        world_t w;
        world_init(&w, 800, 480);
        fxp_frame_t *fr = spawn_empty(&w);
        fxp_frame_set_scroll(fr, 500, 300, -1);   /* view at (300,180)    */

        fxp_expr_item_t items[2];
        fxp_expression_t e;
        memset(items, 0, sizeof(items));
        memset(&e, 0, sizeof(e));
        e.count = 2;
        e.items = items;

        const int32_t nums[4] = { 2, 3, 4, 5 };   /* XLeft/Right/YTop/Bottom */
        const int32_t want[4] = { 300, 700, 180, 420 };
        const char *names[4] = { "XLeftEdge", "XRightEdge",
                                 "YTopEdge", "YBottomEdge" };
        for (int i = 0; i < 4; i++) {
            items[0].object_type = FXP_OT_GAME;   /* game property read   */
            items[0].num = (int16_t)nums[i];
            items[0].int_val = 0;
            items[1].object_type = 0;
            items[1].num = 0;
            fxp_value_t v = fxp_expr_eval(fr, &e, NULL, 0);
            CHECK(fxp_value_as_int(&v) == want[i],
                  names[i]);
            fxp_value_free(&v);
        }
        fxp_frame_destroy(fr);
        free(w.layers[0].name);
        free(w.layers[1].name);
    }

    if (g_failures == 0) {
        printf("== ALL SCROLL TESTS PASSED ==\n");
        return 0;
    }
    printf("== %d TEST(S) FAILED ==\n", g_failures);
    return 1;
}
