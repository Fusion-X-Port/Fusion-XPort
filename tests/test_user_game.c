/*
 * Fusion X-Port — tests/test_user_game.c
 * End-to-end regression test against a REAL Clickteam Fusion-authored
 * MFA (tests/data/eventest.mfa, build 295, two frames — "eventest").
 *
 * Covers the two bugs the author hit with this exact file:
 *   1. "EvOb" object-info mapping: real Fusion references objects by the
 *      event-editor row handle (rows 0 and 2 here map to frame items 0
 *      and 3), so untranslated events targeted the wrong object.
 *   2. Infix expressions: "Set X to X(Active) + 2" is stored INFIX
 *      ([X(Active)][+][2]); the old RPN evaluator underflowed on the
 *      operator and returned the bare constant, teleporting the object
 *      to the top-left corner on every DPAD press.
 *
 * Also exercises the fine-detection (pixel-perfect) collision path and
 * the edge-triggered "is colliding with an object" condition (-14) used
 * by the gem-collection event.
 */
#include "fxp/mfa.h"
#include "fxp/mfa_format.h"
#include "fxp/value.h"
#include "fxp/selector.h"
#include "fxp/frame.h"
#include "fxp/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

/* --- fake audio backend hooks (record what the interpreter plays) ---- */
static struct {
    int      load_calls;
    size_t   load_size;
    uint8_t  load_head[2];
    int      play_calls;
    int      last_channel;
    int      last_loops;
    int      stop_calls;
} g_audio;

static int fake_audio_load(void *audio, const uint8_t *data, size_t size)
{
    (void)audio;
    g_audio.load_calls++;
    g_audio.load_size = size;
    if (size >= 2) {
        g_audio.load_head[0] = data[0];
        g_audio.load_head[1] = data[1];
    }
    return 7;                             /* distinctive mixer slot     */
}

static void fake_audio_play(void *audio, int channel, int loops)
{
    (void)audio;
    g_audio.play_calls++;
    g_audio.last_channel = channel;
    g_audio.last_loops = loops;
}

static void fake_audio_stop_all(void *audio)
{
    (void)audio;
    g_audio.stop_calls++;
}

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            printf("  ok: %s\n", msg);                                      \
        } else {                                                            \
            printf("  FAIL: %s\n", msg);                                    \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static uint8_t *slurp(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static void tick(fxp_frame_t *fr, int32_t times)
{
    for (int32_t i = 0; i < times; i++) {
        fxp_frame_update(fr, 1.0 / 60.0);   /* events + deferred deletes  */
        fxp_runtime_end_tick(fr->rt);
    }
}

int main(void)
{
    printf("== Fusion X-Port: real Fusion-authored MFA (eventest) ==\n");

    size_t size = 0;
    uint8_t *buf = slurp("tests/data/eventest.mfa", &size);
    CHECK(buf != NULL, "eventest.mfa read");

    fxp_mfa_t *mfa = buf ? fxp_mfa_load(buf, size) : NULL;
    CHECK(mfa != NULL, "eventest.mfa loads");
    if (!mfa)
        return 1;

    CHECK(mfa->frame_count == 2, "two frames");
    CHECK(mfa->images.count == 26, "26 images");

    /* ---- EvOb translation ------------------------------------------ */
    fxp_mfa_frame_t *f0 = mfa->frames[0];
    CHECK(f0->events.object_count == 2, "two EvOb rows");
    bool rows_ok =
        f0->events.objects[0].handle == 0 &&
        f0->events.objects[0].item_handle == 0 &&
        f0->events.objects[1].handle == 2 &&
        f0->events.objects[1].item_handle == 3;
    CHECK(rows_ok, "EvOb rows {0->0, 2->3} parsed");

    /* Group 5 ("gem collected"): act[1] is Destroy targeting EvOb row 2,
     * which must translate to frame item handle 3 (GEMDUDE). */
    CHECK(f0->events.groups[5].action_count == 2, "gem group has 2 actions");
    CHECK(f0->events.groups[5].actions[1].num == 24 &&
              f0->events.groups[5].actions[1].object_info == 3,
          "destroy action retargeted to item handle 3 (GEMDUDE)");

    /* The X(Active)+2 expression must still reference object 0. */
    const fxp_param_t *xp = &f0->events.groups[0].actions[0].params[0];
    CHECK(xp->has_expression && xp->expression.count == 3 &&
              xp->expression.items[0].object_type == 2 &&
              xp->expression.items[0].num == 11 &&
              xp->expression.items[0].object_info == 0 &&
              xp->expression.items[1].object_type == 0 &&
              xp->expression.items[1].num == 2 &&
              xp->expression.items[2].num == FXP_EXPR_LONG,
          "DPAD expression is infix X(Active) + 2");

    /* ---- runtime ---------------------------------------------------- */
    fxp_frame_t *fr = fxp_frame_create(mfa, f0);   /* owns its runtime     */
    fxp_runtime_t *rt = fr->rt;
    rt->audio = (void *)1;
    rt->audio_load = fake_audio_load;
    rt->audio_play = fake_audio_play;
    rt->audio_stop_all = fake_audio_stop_all;
    CHECK(fxp_frame_spawn_instances(fr) == 6, "six instances spawned");

    fxp_object_t *hero = NULL;
    for (int32_t i = 0; i < fr->instance_count; i++)
        if (fr->instances[i]->object_info == 0)
            hero = fr->instances[i];
    CHECK(hero && hero->x == 21 && hero->y == 106,
          "hero starts at (21, 106)");

    /* ---- DPAD movement: +/-2 per tick, NOT a teleport --------------- */
    rt->input.keys_down[0x27] = 1;        /* Right                        */
    tick(fr, 10);
    rt->input.keys_down[0x27] = 0;
     CHECK(hero->x == 21 + 20, "Right held 10 ticks -> x += 20 (2 per tick)");
    CHECK(hero->y == 106, "Right leaves y alone");

    rt->input.keys_down[0x25] = 1;        /* Left                         */
    tick(fr, 3);
    rt->input.keys_down[0x25] = 0;
    CHECK(hero->x == 41 - 6, "Left held 3 ticks -> x -= 6");

    rt->input.keys_down[0x26] = 1;        /* Up                           */
    tick(fr, 4);
    rt->input.keys_down[0x26] = 0;
    CHECK(hero->y == 106 - 8, "Up held 4 ticks -> y -= 8");
    CHECK(hero->x == 35, "Up leaves x alone");

    rt->input.keys_down[0x28] = 1;        /* Down                         */
    tick(fr, 2);
    rt->input.keys_down[0x28] = 0;
    CHECK(hero->y == 98 + 4, "Down held 2 ticks -> y += 4");

    /* ---- gem collection: collision edge + destroy -------------------- */
    int32_t before = fr->instance_count;
    fxp_value_t *wincond = fxp_object_value(hero, 0);
    CHECK(wincond && fxp_value_as_int(wincond) == 0, "wincond starts at 0");

    /* Move onto the gem at (129, 74). */
    hero->x = 129;
    hero->y = 74;
    tick(fr, 1);
    CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 1,
          "touching a gem adds 1 to wincond");
    CHECK(fr->instance_count == before - 1, "gem destroyed on touch");

    /* Stand still one tick: the gem is gone, wincond must hold. */
    tick(fr, 1);
    CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 1,
          "wincond holds after the gem is gone");

    /* Second gem: a fresh collision edge must fire again. */
    hero->x = 86;
    hero->y = 30;
    tick(fr, 1);
    CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 2,
          "second gem adds again (edge re-armed)");

    /* Far away from everything: nothing changes. */
    hero->x = 380;
    hero->y = 20;
    tick(fr, 3);
    CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 2,
          "no gems nearby -> no wincond change");

    /* ---- win transition: collect every gem -> jump to the win frame -- *
     * Group 6 is "wincond == 5 -> Jump to frame <plain param 1>".  The
     * plain frame param is the target frame's HANDLE (eventest's frames
     * carry handles 0 and 1, so handle 1 == position 1 == the win
     * screen); the runtime queues the raw value plus the is-handle flag
     * and the app resolves it.  (Older builds treated the value as a
     * position, which landed one scene off on files whose frame handles
     * are 1-based.) */
    while (fxp_value_as_int(fxp_object_value(hero, 0)) < 5) {
        fxp_object_t *gem = NULL;
        for (int32_t i = 0; i < fr->instance_count; i++)
            if (fr->instances[i]->object_info == 3)
                gem = fr->instances[i];
        if (!gem)
            break;                          /* no gems left: bail out   */
        hero->x = gem->x;
        hero->y = gem->y;
        tick(fr, 1);          /* walk onto the gem -> collision starts  */
        tick(fr, 1);          /* gem gone: overlap ends, edge re-arms   */
    }
    CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 5,
          "all five gems collected -> wincond == 5");
    CHECK(fr->rt->pending_state == 3, "win condition queues the frame jump");
    CHECK(fr->rt->pending_frame == 1 && fr->rt->pending_frame_is_handle,
          "jump target is frame handle 1 (eventest win screen)");

    /* ---- sample playback: the sound-handle bug ----------------------- *
     * Frame 0 group 4 is "Upon pressing Space -> Play sample <0>".  The
     * sound bank stores the first item with handle 0 and the param
     * references 0, so the play action must load the bank's 7105-byte
     * MP3 (ff fb frame sync) and start it on the loaded channel.  The
     * old reader subtracted 1 from the bank handle, the lookup never
     * matched, and every game was silent. */
    memset(&g_audio, 0, sizeof(g_audio));
    rt->input.keys_pressed[0x20] = 1;     /* VK_SPACE                     */
    rt->input.any_key_pressed = true;
    tick(fr, 1);
    CHECK(g_audio.load_calls == 1 && g_audio.load_size == 7105,
          "space -> sound bank item 0 loaded once (7105 bytes)");
    CHECK(g_audio.load_head[0] == 0xFF && g_audio.load_head[1] == 0xFB,
          "loaded payload is the MP3 stream (ff fb sync)");
    CHECK(g_audio.play_calls == 1 && g_audio.last_channel == 7 &&
              g_audio.last_loops == 0,
          "space -> PlaySample starts the loaded channel once");
    CHECK(rt->sample_channel[0] == 7,
          "sample handle 0 caches its mixer channel");

    fxp_frame_destroy(fr);
    fxp_mfa_free(mfa);
    free(buf);

    if (g_failures == 0) {
        printf("== ALL USER-GAME TESTS PASSED ==\n");
        return 0;
    }
    printf("== %d TEST(S) FAILED ==\n", g_failures);
    return 1;
}
