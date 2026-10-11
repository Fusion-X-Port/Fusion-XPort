/*
 * Fusion X-Port — tools/make_input_mfa.c
 * Builds a synthetic MFA whose events are driven by INPUT, used to verify
 * the whole input chain end to end:
 *
 *   app input layer (g_pad -> merge_input -> rt->input.keys_down/pressed)
 *     -> keyboard conditions:
 *          group A  "Upon pressing Space"        (-6,-1)  -> Set position (250,150)
 *          group B  "Repeat while DOWN pressed"  (-6,-2)  -> Set Y = Y+2  (expression)
 *          group C  "Repeat while UP pressed"    (-6,-2)  -> Add 1 to Alterable A
 *          group D  "Repeat while LEFT pressed"  (-6,-2)  -> Set X = X-2  (expression)
 *     -> actions (PARAM_POSITION / expression / alterable add)
 *
 * Groups B..D mirror how Fusion users author D-pad movement ("repeat
 * while key is pressed" + relative Set X/Y) — the pattern that regressed
 * unnoticed on hardware because only "upon pressing" had a test.
 *
 * Two modes:
 *   1. Logic test (default): loads the built MFA, injects synthetic key
 *      states into the runtime input, ticks the interpreter and asserts
 *      the actions run — every tick for "repeat while", once for
 *      "upon pressing", and not at all without input.
 *   2. FXP_INPUT_MFA=<path>: also writes the MFA to disk so
 *      `fxp_host <path>` + FXP_HOLD_KEY=<vk>:<from>:<to> + FXP_SNAPSHOT=N
 *      can verify the SDL/app-level path with a pixel check.
 *
 * Host-only build; links the same sources as the runtime.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zlib.h>

#include "fxp/fxp.h"
#include "fxp/mfa.h"
#include "fxp/mfa_format.h"
#include "fxp/frame.h"
#include "fxp/runtime.h"

/* Fusion key codes used by the fixture (Windows VK) */
#define VK_UP    0x26
#define VK_LEFT  0x25
#define VK_DOWN  0x28
#define VK_SPACE 0x20

/* ------------------------------------------------------------ writer ---- */

typedef struct { uint8_t *data; size_t size, cap; } wr_t;

static void wr_init(wr_t *w) { w->data = NULL; w->size = w->cap = 0; }
static void wr_put(wr_t *w, const void *p, size_t n)
{
    if (w->size + n > w->cap) {
        w->cap = w->cap ? w->cap * 2 : 256;
        while (w->size + n > w->cap) w->cap *= 2;
        w->data = realloc(w->data, w->cap);
    }
    memcpy(w->data + w->size, p, n);
    w->size += n;
}
static void wr_i16(wr_t *w, int16_t v)  { wr_put(w, &v, 2); }
static void wr_u16(wr_t *w, uint16_t v) { wr_put(w, &v, 2); }
static void wr_i32(wr_t *w, int32_t v)  { wr_put(w, &v, 4); }
static void wr_u32(wr_t *w, uint32_t v) { wr_put(w, &v, 4); }
static void wr_f32(wr_t *w, float v)    { wr_put(w, &v, 4); }
static void wr_u8(wr_t *w, uint8_t v)   { wr_put(w, &v, 1); }
static void wr_skip(wr_t *w, size_t n)
{
    static const uint8_t z[64] = { 0 };
    while (n) { size_t c = n > 64 ? 64 : n; wr_put(w, z, c); n -= c; }
}
static void wr_tag(wr_t *w, const char *t) { wr_put(w, t, 4); }
static void wr_color(wr_t *w, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t c[4] = { r, g, b, a };
    wr_put(w, c, 4);
}
/* AutoUnicode: i16 len + i16 0x8000 marker + UTF-16LE */
static void wr_auto_unicode(wr_t *w, const char *utf8)
{
    size_t n = strlen(utf8);
    wr_i16(w, (int16_t)n);
    wr_i16(w, (int16_t)0x8000);
    for (size_t i = 0; i < n; i++)
        wr_u16(w, (uint16_t)(unsigned char)utf8[i]);
}

/* AGMI image item: `dim`x`dim` solid block, mode 4 (B,G,R order!), zlib.
 * Layout (CTFAK ImageTranslator rules): ALL BGR rows first
 * (stride w*3, even w -> no pad), then the whole alpha plane
 * (rows padded to 4 bytes). */
static void build_solid_image(wr_t *w, int32_t handle, int32_t dim,
                              uint8_t r, uint8_t g, uint8_t b)
{
    size_t bgr_row = (size_t)dim * 3;
    size_t alpha_row = (size_t)dim;
    size_t raw_len = (bgr_row + alpha_row) * (size_t)dim;
    uint8_t *raw = malloc(raw_len);
    uint8_t *bgr = raw;
    uint8_t *alpha = raw + bgr_row * (size_t)dim;
    for (int32_t y = 0; y < dim; y++) {
        uint8_t *row = bgr + (size_t)y * bgr_row;
        for (int32_t x = 0; x < dim; x++) {
            row[x * 3 + 0] = b;               /* MFA mode-4 stores B,G,R */
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        memset(alpha + (size_t)y * alpha_row, 255, alpha_row);
    }

    uLongf cap = (uLongf)raw_len + 64;
    uint8_t *comp = malloc(cap);
    uLongf clen = cap;
    assert(compress2(comp, &clen, raw, raw_len, Z_DEFAULT_COMPRESSION) == Z_OK);

    wr_i32(w, handle);
    wr_i32(w, 0x1234);                     /* checksum                     */
    wr_i32(w, 1);                          /* references                   */
    wr_i32(w, (int32_t)(clen + 4));        /* innerSize = compressed + 4   */
    wr_i16(w, (int16_t)dim);
    wr_i16(w, (int16_t)dim);
    wr_u8(w, FXP_GM_24BIT);                /* graphic mode 4               */
    wr_u8(w, FXP_IMGFLAG_LZX | FXP_IMGFLAG_ALPHA);
    wr_i16(w, 0);                          /* reserved                     */
    wr_i16(w, (int16_t)(dim / 2));         /* hotspot: center              */
    wr_i16(w, (int16_t)(dim / 2));
    wr_i16(w, 0);                          /* action x                     */
    wr_i16(w, 0);                          /* action y                     */
    wr_color(w, 0, 0, 0, 255);             /* transparent                  */
    wr_i32(w, (int32_t)raw_len);           /* decompSize                   */
    wr_put(w, comp, clen);
    free(comp);
    free(raw);
}

/* ObjectLoader common header shared by all item types. */
static void wr_common(wr_t *w, uint32_t new_flags)
{
    wr_i32(w, 0);                          /* object flags                 */
    wr_i32(w, (int32_t)new_flags);         /* new flags                    */
    wr_color(w, 255, 255, 255, 255);       /* back color                   */
    for (int i = 0; i < 8; i++) wr_i16(w, -1);
    wr_i16(w, -1);                         /* qualifier terminator         */
    wr_i32(w, 0);                          /* values count                 */
    wr_i32(w, 0);                          /* strings count                */
    wr_u32(w, 0);                          /* movements count              */
    wr_i32(w, 0);                          /* behaviours count             */
    wr_skip(w, 2);                         /* transitions                  */
}

/* Active "Mover" (handle 3): static movement, Stopped anim -> image 7. */
static void build_active_item(wr_t *w)
{
    wr_i32(w, FXP_OT_ACTIVE);
    wr_i32(w, 3);                          /* handle = object info         */
    wr_auto_unicode(w, "Mover");
    wr_i32(w, (int32_t)0xFFFFFFFF);        /* transparent                  */
    wr_i32(w, 0);                          /* ink effect                   */
    wr_u32(w, 255);                        /* ink param                    */
    wr_i32(w, 0);                          /* antialiasing                 */
    wr_i32(w, 0);                          /* flags                        */
    wr_i32(w, 1);                          /* icon type                    */
    wr_i32(w, 0);                          /* icon handle                  */
    wr_u8(w, 0);                           /* mini-chunks terminator       */
    wr_common(w, 1 << 3);                  /* VisibleAtStart               */
    /* animations: "Stopped" with one direction, frames [7] */
    wr_u8(w, 1);                           /* has anims                    */
    wr_u32(w, 1);                          /* anim count                   */
    wr_auto_unicode(w, "Stopped");
    wr_i32(w, 1);                          /* direction count              */
    wr_i32(w, 0);                          /* index                        */
    wr_i32(w, 25);                         /* min speed                    */
    wr_i32(w, 50);                         /* max speed                    */
    wr_i32(w, 1);                          /* repeat                       */
    wr_i32(w, 0);                          /* back to                      */
    wr_i32(w, 1);                          /* frame count                  */
    wr_i32(w, 7);                          /* frame -> image 7             */
}

/* --------------------------------------------------- event-group writer -- */

/* Condition header: 14 bytes (with trailing identifier). */
static void wr_cond_header(wr_t *c, int16_t type, int16_t num)
{
    wr_i16(c, type);
    wr_i16(c, num);
    wr_u16(c, 0);                          /* object info                  */
    wr_i16(c, 0);                          /* object info list             */
    wr_u8(c, 0);                           /* flags                        */
    wr_u8(c, 0);                           /* other flags                  */
    wr_u8(c, 1);                           /* nparams                      */
    wr_u8(c, 0);                           /* def type                     */
    wr_i16(c, 0);                          /* identifier                   */
}

/* PARAM_KEY (code 14): u16 VK code. */
static void wr_cond_key(wr_t *c, uint16_t vk)
{
    wr_i16(c, 6);                          /* param size incl. itself      */
    wr_i16(c, FXP_PARAM_KEY);
    wr_u16(c, vk);
}

/* Action header: 12 bytes (no identifier). nparams passed by caller. */
static void wr_action_header(wr_t *a, int16_t type, int16_t num,
                             uint16_t objinfo, uint8_t nparams)
{
    wr_i16(a, type);
    wr_i16(a, num);
    wr_u16(a, objinfo);
    wr_i16(a, 0);                          /* object info list             */
    wr_u8(a, 0);                           /* flags                        */
    wr_u8(a, 0);                           /* other flags                  */
    wr_u8(a, nparams);
    wr_u8(a, 0);                           /* def type                     */
}

/* Expression: one object property, one int constant, one operator, in
 * Fusion's INFIX token order (property, operator, constant — verified
 * against real Fusion-authored MFAs).  Property nums follow Fusion:
 * Y=1, X=11.  Operators: add=2, sub=4. */
static void wr_expr_prop_op_const(wr_t *w, int16_t prop, uint16_t objinfo,
                                  int16_t op, int32_t cst)
{
    wr_i16(w, 0);                          /* comparison                   */
    /* object property item */
    wr_i16(w, 2);                          /* object type: Active          */
    wr_i16(w, prop);
    wr_i16(w, 14);                         /* item size incl. header       */
    wr_u16(w, objinfo);
    wr_i16(w, 0);                          /* object info list             */
    wr_i32(w, 0);                          /* unknown dword (Expr.cs)      */
    /* operator (infix: between the operands) */
    wr_i16(w, 0);                          /* arithmetic                   */
    wr_i16(w, op);
    wr_i16(w, 6);
    /* int constant */
    wr_i16(w, -1);                         /* system                       */
    wr_i16(w, FXP_EXPR_LONG);
    wr_i16(w, 10);
    wr_i32(w, cst);
    /* terminator */
    wr_i16(w, 0); wr_i16(w, 0);
}

/* Expression: single int constant (used by the alterable add). */
static void wr_expr_const(wr_t *w, int32_t cst)
{
    wr_i16(w, 0);                          /* comparison                   */
    wr_i16(w, -1);
    wr_i16(w, FXP_EXPR_LONG);
    wr_i16(w, 10);
    wr_i32(w, cst);
    wr_i16(w, 0); wr_i16(w, 0);
}

static void wr_expression_param(wr_t *a, const wr_t *expr)
{
    wr_i16(a, (int16_t)(expr->size + 4));  /* param size incl. itself      */
    wr_i16(a, FXP_PARAM_EXPRESSION);
    wr_put(a, expr->data, expr->size);
}

static void wr_group(wr_t *frame, const wr_t *body)
{
    /* group record: negative total size incl. the size field (MFA rule) */
    wr_i16(frame, (int16_t)(-(int16_t)(body->size + 2)));
    wr_put(frame, body->data, body->size);
}

static void wr_group_header(wr_t *body)
{
    wr_u8(body, 1);                        /* ncond                        */
    wr_u8(body, 1);                        /* nact                         */
    wr_u16(body, 0);                       /* flags                        */
    wr_i16(body, 0);                       /* is restricted                */
    wr_i16(body, 0);                       /* restrict cpt                 */
    wr_i16(body, 0);                       /* identifier                   */
    wr_i16(body, 0);                       /* undo                         */
}

/*
 * Four event groups (see file header):
 *   A: upon Space  -> Set position of Mover to (250,150)
 *   B: repeat DOWN -> Set Y position of Mover to Y(Mover)+2
 *   C: repeat UP   -> Add 1 to Alterable Value A of Mover
 *   D: repeat LEFT -> Set X position of Mover to X(Mover)-2
 */
static void write_events(wr_t *frame)
{
    wr_t body; wr_init(&body);

    /* ---- group A: "Upon pressing Space" (type -6, num -1) ---- */
    {
        wr_t c; wr_init(&c);
        wr_cond_header(&c, -6, -1);
        wr_cond_key(&c, VK_SPACE);

        wr_t a; wr_init(&a);
        wr_action_header(&a, FXP_OT_ACTIVE, 1, 3, 1);   /* set position   */
        wr_i16(&a, 12);                                 /* position param */
        wr_i16(&a, FXP_PARAM_POSITION);
        wr_u16(&a, 0xFFFF);                             /* parent: none   */
        wr_i16(&a, 0);                                  /* (unused)       */
        wr_i16(&a, 250);
        wr_i16(&a, 150);

        wr_t g; wr_init(&g);
        wr_group_header(&g);
        wr_u16(&g, (uint16_t)(c.size + 2)); wr_put(&g, c.data, c.size);
        wr_u16(&g, (uint16_t)(a.size + 2)); wr_put(&g, a.data, a.size);
        wr_group(&body, &g);
        free(g.data); free(a.data); free(c.data);
    }

    /* ---- group B: "Repeat while key DOWN is pressed" (-6,-2) ----
     * action: Set Y position to Y(Mover)+2  (expression movement)      */
    {
        wr_t c; wr_init(&c);
        wr_cond_header(&c, -6, -2);
        wr_cond_key(&c, VK_DOWN);

        wr_t e; wr_init(&e);
        wr_expr_prop_op_const(&e, 1 /*Y*/, 3, 2 /*+*/, 2);

        wr_t a; wr_init(&a);
        wr_action_header(&a, FXP_OT_ACTIVE, 3 /*set Y*/, 3, 1);
        wr_expression_param(&a, &e);

        wr_t g; wr_init(&g);
        wr_group_header(&g);
        wr_u16(&g, (uint16_t)(c.size + 2)); wr_put(&g, c.data, c.size);
        wr_u16(&g, (uint16_t)(a.size + 2)); wr_put(&g, a.data, a.size);
        wr_group(&body, &g);
        free(g.data); free(a.data); free(e.data); free(c.data);
    }

    /* ---- group C: "Repeat while key UP is pressed" (-6,-2) ----
     * action: Add 1 to Alterable Value A (fires EVERY tick while held) */
    {
        wr_t c; wr_init(&c);
        wr_cond_header(&c, -6, -2);
        wr_cond_key(&c, VK_UP);

        wr_t e; wr_init(&e);
        wr_expr_const(&e, 1);

        wr_t a; wr_init(&a);
        wr_action_header(&a, FXP_OT_ACTIVE, 32 /*add alterable*/, 3, 2);
        wr_i16(&a, 6);                                  /* alterable idx  */
        wr_i16(&a, FXP_PARAM_ALTERABLE);
        wr_i16(&a, 0);                                  /* Alterable A    */
        wr_expression_param(&a, &e);

        wr_t g; wr_init(&g);
        wr_group_header(&g);
        wr_u16(&g, (uint16_t)(c.size + 2)); wr_put(&g, c.data, c.size);
        wr_u16(&g, (uint16_t)(a.size + 2)); wr_put(&g, a.data, a.size);
        wr_group(&body, &g);
        free(g.data); free(a.data); free(e.data); free(c.data);
    }

    /* ---- group D: "Repeat while key LEFT is pressed" (-6,-2) ----
     * action: Set X position to X(Mover)-2                             */
    {
        wr_t c; wr_init(&c);
        wr_cond_header(&c, -6, -2);
        wr_cond_key(&c, VK_LEFT);

        wr_t e; wr_init(&e);
        wr_expr_prop_op_const(&e, 11 /*X*/, 3, 4 /*-*/, 2);

        wr_t a; wr_init(&a);
        wr_action_header(&a, FXP_OT_ACTIVE, 2 /*set X*/, 3, 1);
        wr_expression_param(&a, &e);

        wr_t g; wr_init(&g);
        wr_group_header(&g);
        wr_u16(&g, (uint16_t)(c.size + 2)); wr_put(&g, c.data, c.size);
        wr_u16(&g, (uint16_t)(a.size + 2)); wr_put(&g, a.data, a.size);
        wr_group(&body, &g);
        free(g.data); free(a.data); free(e.data); free(c.data);
    }

    /* Evts payload = the group records, concatenated (no extra framing) */
    wr_put(frame, body.data, body.size);
    free(body.data);
}

static void build_frame(wr_t *frame, wr_t *item_active)
{
    wr_t ev; wr_init(&ev);
    write_events(&ev);

    wr_i32(frame, 0);                      /* handle                       */
    wr_auto_unicode(frame, "InputTest");
    wr_i32(frame, 400);                    /* size x                       */
    wr_i32(frame, 240);                    /* size y                       */
    wr_color(frame, 24, 24, 28, 255);      /* background                   */
    wr_u32(frame, 0);                      /* flags                        */
    wr_i32(frame, 100);                    /* max objects                  */
    wr_i32(frame, 0);                      /* garbage                      */
    wr_i32(frame, 0);                      /* password len                 */
    wr_i32(frame, 0);                      /* last viewed x                */
    wr_i32(frame, 0);                      /* last viewed y                */
    wr_i32(frame, 256);                    /* palette size                 */
    for (int i = 0; i < 256; i++) wr_color(frame, 0, 0, 0, 255);
    wr_i32(frame, 0);                      /* stamp handle                 */
    wr_i32(frame, 0);                      /* active layer                 */
    wr_i32(frame, 1);                      /* layer count                  */
    wr_auto_unicode(frame, "Layer 1");
    wr_u32(frame, 1);                      /* flags: visible               */
    wr_f32(frame, 1.0f);                   /* x coeff                      */
    wr_f32(frame, 1.0f);                   /* y coeff                      */
    wr_u8(frame, 0);                       /* fade in flag                 */
    wr_u8(frame, 0);                       /* fade out flag                */
    wr_i32(frame, 1);                      /* item count                   */
    wr_put(frame, item_active->data, item_active->size);
    wr_i32(frame, 0);                      /* folder count                 */
    wr_i32(frame, 1);                      /* instance count               */
    /* instance: Mover at (50,50) */
    wr_i32(frame, 50);  wr_i32(frame, 50);
    wr_u32(frame, 0);   wr_i32(frame, 1);  /* layer, handle                */
    wr_i16(frame, 0);   wr_i16(frame, 0);  /* flags, instance              */
    wr_u32(frame, 0);   wr_u32(frame, 3);  /* parentType, itemHandle       */
    wr_i32(frame, -1);                     /* parentHandle                 */
    /* MFAEvents: version/frameType + "Evts" block + "!DNE" */
    wr_u16(frame, 2);
    wr_u16(frame, 0);
    wr_tag(frame, "Evts");
    wr_u32(frame, (uint32_t)ev.size);
    wr_put(frame, ev.data, ev.size);
    wr_tag(frame, "!DNE");
    wr_u8(frame, 0);                       /* mini-chunks terminator       */
    free(ev.data);
}

/* --------------------------------------------------------------- main ---- */

static int g_failed = 0;
#define CHECK(cond, msg) do { \
        printf("  %s: %s\n", (cond) ? "ok" : "FAIL", msg); \
        if (!(cond)) g_failed++; \
    } while (0)

/* Tick the interpreter `n` times with exactly `vk` held (merge-style:
 * keys_down is rebuilt from scratch every frame, so each phase starts
 * with a clean keyboard — holding UP does not leak into the next phase). */
static void run_ticks(fxp_frame_t *fr, int32_t n, int vk)
{
    for (int32_t i = 0; i < n; i++) {
        memset(fr->rt->input.keys_down, 0, 256);
        if (vk > 0) {
            fr->rt->input.keys_down[vk] = 1;
            if (i == 0) {
                fr->rt->input.keys_pressed[vk] = 1;
                fr->rt->input.any_key_pressed = true;
            }
        }
        fxp_runtime_tick(fr, 1.0 / 60.0);
        fxp_runtime_end_tick(fr->rt);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Fusion X-Port: input event round-trip ==\n");

    /* image bank: [7] = green 32x32 (the Mover), [8] = unused spare */
    wr_t imagebank; wr_init(&imagebank);
    wr_i32(&imagebank, 4);                 /* AGMI graphic mode            */
    wr_i16(&imagebank, 0);                 /* palette version              */
    wr_i16(&imagebank, 0);                 /* palette entries              */
    wr_i32(&imagebank, 2);                 /* image count                  */
    build_solid_image(&imagebank, 7, 32, 0, 255, 0);
    build_solid_image(&imagebank, 8, 8, 255, 0, 0);

    wr_t item_active;
    wr_init(&item_active); build_active_item(&item_active);

    wr_t frame; wr_init(&frame);
    build_frame(&frame, &item_active);
    free(item_active.data);

    /* app (mirrors the round-trip fixture layout) */
    wr_t mfa; wr_init(&mfa);
    wr_tag(&mfa, "MFU2");
    wr_i16(&mfa, 770);
    wr_i16(&mfa, 772);
    wr_i32(&mfa, 287);
    wr_i32(&mfa, 291);
    wr_i32(&mfa, 0);
    wr_auto_unicode(&mfa, "Input Test");
    wr_auto_unicode(&mfa, "Input-driven fixture");
    wr_auto_unicode(&mfa, "C:\\Games\\Input");
    wr_i32(&mfa, 0);                       /* stamp                        */
    wr_tag(&mfa, "ATNF");
    wr_i32(&mfa, 0);                       /* fonts                        */
    wr_tag(&mfa, "APMS");
    wr_i32(&mfa, 0);                       /* sounds                       */
    wr_tag(&mfa, "ASUM");
    wr_i32(&mfa, 0);                       /* music                        */
    wr_tag(&mfa, "AGMI");                  /* icon bank                    */
    wr_i32(&mfa, 4);
    wr_i16(&mfa, 0); wr_i16(&mfa, 0);
    wr_i32(&mfa, 0);
    wr_tag(&mfa, "AGMI");                  /* image bank                   */
    wr_put(&mfa, imagebank.data, imagebank.size);
    free(imagebank.data);

    wr_auto_unicode(&mfa, "Input Test");   /* name (repeat)                */
    wr_auto_unicode(&mfa, "Author");
    wr_auto_unicode(&mfa, "Input-driven fixture");
    wr_auto_unicode(&mfa, "Copyright");
    wr_auto_unicode(&mfa, "Company");
    wr_auto_unicode(&mfa, "1.0");
    wr_i32(&mfa, 400);
    wr_i32(&mfa, 240);
    wr_color(&mfa, 0, 0, 0, 255);
    wr_u32(&mfa, 0);
    wr_u32(&mfa, 0);
    wr_auto_unicode(&mfa, "");             /* help file                    */
    wr_auto_unicode(&mfa, "");             /* unknown                      */
    wr_i32(&mfa, 0);                       /* initial score                */
    wr_i32(&mfa, 5);                       /* initial lives                */
    wr_i32(&mfa, 60);                      /* frame rate                   */
    wr_i32(&mfa, 0);                       /* build type                   */
    wr_auto_unicode(&mfa, "");             /* build path                   */
    wr_auto_unicode(&mfa, "");             /* unknown 2                    */
    wr_auto_unicode(&mfa, "");             /* command line                 */
    wr_auto_unicode(&mfa, "");             /* about box                    */
    wr_u32(&mfa, 0);
    wr_i32(&mfa, 0);                       /* binary files                 */
    wr_i32(&mfa, 4);                       /* controls: 4 players          */
    for (int p = 0; p < 4; p++) {
        wr_i32(&mfa, p == 0 ? 1 : 0);
        wr_i32(&mfa, 16);
        wr_i32(&mfa, 38); wr_i32(&mfa, 40); wr_i32(&mfa, 37); wr_i32(&mfa, 39);
        wr_i32(&mfa, 16); wr_i32(&mfa, 17); wr_i32(&mfa, 32); wr_i32(&mfa, 13);
        for (int u = 0; u < 8; u++) wr_i32(&mfa, 0);
    }
    wr_u32(&mfa, 0);                       /* menu size                    */
    wr_i32(&mfa, 0);                       /* window menu index            */
    wr_i32(&mfa, 0);                       /* menu images                  */
    wr_i32(&mfa, 0);                       /* global values                */
    wr_i32(&mfa, 0);                       /* global strings               */
    wr_i32(&mfa, 0);                       /* global events                */
    wr_i32(&mfa, 4);                       /* graphic mode                 */
    wr_i32(&mfa, 0);                       /* icons                        */
    wr_i32(&mfa, 0);                       /* quals                        */
    wr_i32(&mfa, 0);                       /* extensions                   */

    size_t off_pos = mfa.size;
    wr_i32(&mfa, 1);                       /* frame offset count           */
    wr_i32(&mfa, 0);                       /* placeholder frame offset     */
    size_t next_pos = mfa.size;
    wr_i32(&mfa, 0);                       /* placeholder next offset      */
    int32_t frame_offset = (int32_t)mfa.size;
    wr_put(&mfa, frame.data, frame.size);
    free(frame.data);
    int32_t next_offset = (int32_t)mfa.size;
    wr_u8(&mfa, 0);                        /* app chunks terminator        */
    memcpy(mfa.data + off_pos + 4, &frame_offset, 4);
    memcpy(mfa.data + next_pos, &next_offset, 4);

    /* export for the SDL/app-level end-to-end run */
    const char *out = getenv("FXP_INPUT_MFA");
    if (out) {
        FILE *fp = fopen(out, "wb");
        if (fp) {
            fwrite(mfa.data, 1, mfa.size, fp);
            fclose(fp);
            printf("  input MFA written to %s (%zu bytes)\n", out, mfa.size);
        }
    }

    /* ---- load & assert ---------------------------------------------------- */
    fxp_mfa_t *game = fxp_mfa_load(mfa.data, mfa.size);
    free(mfa.data);

    CHECK(game != NULL, "mfa loads");
    if (!game)
        return g_failed ? 1 : 0;

    CHECK(game->frame_count == 1, "one frame");
    const fxp_mfa_frame_t *f0 = game->frames[0];
    CHECK(f0->events.group_count == 4, "four event groups");
    if (f0->events.group_count == 4) {
        const fxp_event_group_t *gr = &f0->events.groups[0];
        CHECK(gr->condition_count == 1 && gr->action_count == 1,
              "group A: 1 condition / 1 action");
        const fxp_condition_t *cd = &gr->conditions[0];
        CHECK(cd->object_type == -6 && cd->num == -1,
              "group A: keyboard 'upon pressing'");
        CHECK(cd->param_count == 1 &&
              cd->params[0].code == FXP_PARAM_KEY, "group A: key param");

        for (int i = 1; i < 4; i++) {
            const fxp_event_group_t *g = &f0->events.groups[i];
            const fxp_condition_t *c = &g->conditions[0];
            CHECK(g->condition_count == 1 && g->action_count == 1,
                  "repeat-while group parses");
            CHECK(c->object_type == -6 && c->num == -2,
                  "condition is keyboard 'repeat while pressed'");
            CHECK(c->param_count == 1 &&
                  c->params[0].code == FXP_PARAM_KEY,
                  "repeat-while: key param present");
        }
        CHECK(f0->events.groups[1].conditions[0].params[0].raw &&
              f0->events.groups[1].conditions[0].params[0].raw_size >= 6,
              "group B: key param payload");
        CHECK(f0->events.groups[2].actions[0].num == 32,
              "group C: add-to-alterable action");
        CHECK(f0->events.groups[1].actions[0].num == 3 &&
              f0->events.groups[1].actions[0].params[0].has_expression,
              "group B: set-Y action with expression param");
    }

    /* interpreter-level test: inject synthetic input, tick, verify */
    fxp_frame_t *fr = fxp_frame_create(game, f0);
    fxp_frame_spawn_instances(fr);
    CHECK(fr->rt != NULL && fr->instance_count == 1, "frame + runtime ready");
    if (fr->rt && fr->instance_count == 1) {
        fxp_object_t *hero = fr->instances[0];
        int32_t x0 = hero->x, y0 = hero->y;

        /* 1. tick WITHOUT input: nothing must move, nothing must count */
        run_ticks(fr, 60, 0);
        CHECK(hero->x == x0 && hero->y == y0,
              "no input -> position untouched after 60 ticks");
        CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 0,
              "no input -> alterable counter stays 0");

        /* 2. hold UP for 60 ticks: "repeat while" must fire EVERY tick */
        run_ticks(fr, 60, VK_UP);
        CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 60,
              "hold UP 60 ticks -> alterable counter == 60 (fires every tick)");
        CHECK(hero->x == x0 && hero->y == y0,
              "hold UP -> position untouched (isolation)");

        /* 3. hold DOWN for 30 ticks: Set Y = Y+2 every tick */
        run_ticks(fr, 30, VK_DOWN);
        CHECK(hero->y == y0 + 60,
              "hold DOWN 30 ticks -> Y moved +60 (2 per tick)");
         CHECK(fxp_value_as_int(fxp_object_value(hero, 0)) == 60,
              "counter unchanged by DOWN (isolation)");
    }
    fxp_frame_destroy(fr);

    /* fresh runtime for the remaining assertions (clean counters) */
    fr = fxp_frame_create(game, f0);
    fxp_frame_spawn_instances(fr);
    if (fr->rt && fr->instance_count == 1) {
        fxp_object_t *hero = fr->instances[0];
        int32_t x0 = hero->x;

        /* 4. hold LEFT for 20 ticks: Set X = X-2 every tick */
        run_ticks(fr, 20, VK_LEFT);
        CHECK(hero->x == x0 - 40,
              "hold LEFT 20 ticks -> X moved -40 (2 per tick)");

        /* 5. release: nothing further happens */
        int32_t cx = hero->x, cy = hero->y;
        int32_t cnt = fxp_value_as_int(fxp_object_value(hero, 0));
        run_ticks(fr, 10, 0);
        CHECK(hero->x == cx && hero->y == cy &&
              fxp_value_as_int(fxp_object_value(hero, 0)) == cnt,
              "release -> nothing moves, counter holds");
    }
    fxp_frame_destroy(fr);
    fxp_mfa_free(game);

    printf("== %s ==\n", g_failed ? "TESTS FAILED" : "ALL INPUT TESTS PASSED");
    return g_failed ? 1 : 0;
}
