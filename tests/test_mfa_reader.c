/*
 * Fusion X-Port — tests/test_mfa_reader.c
 * Round-trip test: builds a minimal but layout-exact MFA in memory (the
 * writer mirrors CTFAK's FTDecompile/MFAData.Write field order), loads it
 * with the runtime reader, and asserts app/frames/banks/objects/events
 * plus the selector/value runtime semantics.
 *
 * Build + run via `make -f Makefile.host test` (host only).
 */
#include "fxp/mfa.h"
#include "fxp/mfa_format.h"
#include "fxp/value.h"
#include "fxp/selector.h"
#include "fxp/frame.h"

#include <zlib.h>
#include <assert.h>

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

static int report(void)
{
    if (g_failures == 0) {
        printf("== ALL TESTS PASSED ==\n");
        return 0;
    }
    printf("== %d FAILURE(S) ==\n", g_failures);
    return 1;
}

/* ------------------------------------------------------------ byte writer */

typedef struct {
    uint8_t *data;
    size_t   size, cap;
} wr_t;

static void wr_init(wr_t *w)  { w->data = NULL; w->size = w->cap = 0; }
static void wr_put(wr_t *w, const void *p, size_t n)
{
    if (n == 0)
        return;
    if (w->size + n > w->cap) {
        w->cap = w->cap ? w->cap * 2 : 256;
        while (w->cap < w->size + n) w->cap *= 2;
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
    for (size_t i = 0; i < n; i++) wr_u8(w, 0);
}
static void wr_tag(wr_t *w, const char *t) { wr_put(w, t, 4); }
static void wr_color(wr_t *w, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t c[4] = { r, g, b, a };
    wr_put(w, c, 4);
}

/* AutoUnicode: i16 len + 0x8000 marker + UTF-16LE code units. */
static void wr_auto_unicode(wr_t *w, const char *utf8)
{
    size_t n = strlen(utf8);
    if (n > 32767) n = 32767;
    wr_i16(w, (int16_t)n);
    wr_i16(w, (int16_t)0x8000);
    for (size_t i = 0; i < n; i++)
        wr_u16(w, (uint16_t)(unsigned char)utf8[i]);
}

/* Yuniversal with explicit length prefix (i16 + UTF-16LE).
 * Kept for future fixture extensions (font face names, extension data). */
#if 0
static void wr_wide_n(wr_t *w, const char *utf8, int32_t codeunits)
{
    wr_i16(w, (int16_t)codeunits);
    for (int32_t i = 0; i < codeunits; i++)
        wr_u16(w, i < (int32_t)strlen(utf8) ? (uint16_t)(unsigned char)utf8[i] : 0);
}

/* zlib-compress and append (plain stream, no header). */
static void wr_zlib_raw(wr_t *w, const uint8_t *raw, size_t raw_len)
{
    uLongf cap = (uLongf)(raw_len + 64);
    uint8_t *buf = malloc(cap);
    uLongf clen = cap;
    assert(compress2(buf, &clen, raw, raw_len, Z_DEFAULT_COMPRESSION) == Z_OK);
    wr_put(w, buf, clen);
    free(buf);
}
#endif

/* ---------------------------------------------------------- MFA fragments */

/* One AGMI image item: 2x2, mode 4, zlib, with alpha plane.
 * Raw layout (CTFAK ImageTranslator rules):
 *   BGR  rows: w*3 + (w odd ? 3 : 0) bytes each        -> 6 bytes x 2
 *        (the MFA stores mode-4 pixels B,G,R — see decode_mode4)
 *   alpha rows: w + ((4 - w%4) % 4) bytes each          -> 4 bytes x 2
 */
static void build_image_item(wr_t *w)
{
    /* B,G,R triplets: pixel(0,0) is true RED (255,0,0). */
    static const uint8_t pixels[12] = { 0,0,255, 0,255,0, 255,0,0, 0,255,255 };
    /* rows padded to 4-byte stride: [255,128,0,0] and [64,0,0,0] */
    static const uint8_t mask[8] = { 255,128,0,0, 64,0,0,0 };

    uint8_t raw[sizeof(pixels) + sizeof(mask)];
    memcpy(raw, pixels, sizeof(pixels));
    memcpy(raw + sizeof(pixels), mask, sizeof(mask));

    uLongf cap = sizeof(raw) + 64;
    uint8_t *comp = malloc(cap);
    uLongf clen = cap;
    assert(compress2(comp, &clen, raw, sizeof(raw), Z_DEFAULT_COMPRESSION) == Z_OK);

    wr_i32(w, 7);                          /* handle                       */
    wr_i32(w, 0x1234);                     /* checksum                     */
    wr_i32(w, 1);                          /* references                   */
    wr_i32(w, (int32_t)(clen + 4));        /* innerSize = compressed + 4   */
    wr_i16(w, 2);                          /* width                        */
    wr_i16(w, 2);                          /* height                       */
    wr_u8(w, FXP_GM_24BIT);                /* graphic mode 4               */
    wr_u8(w, FXP_IMGFLAG_LZX | FXP_IMGFLAG_ALPHA);
    wr_i16(w, 0);                          /* reserved                     */
    wr_i16(w, 1);                          /* hotspot x                    */
    wr_i16(w, 2);                          /* hotspot y                    */
    wr_i16(w, 0);                          /* action x                     */
    wr_i16(w, 0);                          /* action y                     */
    wr_color(w, 0, 0, 0, 255);             /* transparent                  */
    wr_i32(w, (int32_t)sizeof(raw));       /* decompSize                   */
    wr_put(w, comp, clen);
    free(comp);
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

/* One event group: system "always" condition + system action with a short
 * parameter.  Written per CTFAK EventGroup/Write rules (negative size). */
static void write_events(wr_t *frame)
{
    /* group body (everything after the i16 size field) */
    wr_t body; wr_init(&body);

    wr_u8(&body, 1);                       /* ncond                        */
    wr_u8(&body, 1);                       /* nact                         */
    wr_u16(&body, 0);                      /* flags                        */
    wr_i16(&body, 0);                      /* is restricted                */
    wr_i16(&body, 0);                      /* restrict cpt                 */
    wr_i16(&body, 0);                      /* identifier                   */
    wr_i16(&body, 0);                      /* undo                         */

    /* condition: system always, no params (size = payload + 2) */
    {
        wr_t c; wr_init(&c);
        wr_i16(&c, FXP_OT_SYSTEM);         /* object type                  */
        wr_i16(&c, -1);                    /* num: Always (-1,-1)          */
        wr_u16(&c, 0);                     /* object info                  */
        wr_i16(&c, 0);                     /* object info list             */
        wr_u8(&c, 0);                      /* flags                        */
        wr_u8(&c, 0);                      /* other flags                  */
        wr_u8(&c, 0);                      /* nparams                      */
        wr_u8(&c, 0);                      /* def type                     */
        wr_i16(&c, 0);                     /* identifier                   */
        wr_u16(&body, (uint16_t)(c.size + 2));
        wr_put(&body, c.data, c.size);
        free(c.data);
    }
    /* action: system num 4, one short param (code 3, value 1) */
    {
        wr_t a; wr_init(&a);
        wr_i16(&a, FXP_OT_SYSTEM);
        wr_i16(&a, 4);
        wr_u16(&a, 0);
        wr_i16(&a, 0);
        wr_u8(&a, 0);
        wr_u8(&a, 0);
        wr_u8(&a, 1);                      /* nparams                      */
        wr_u8(&a, 0);                      /* def type                     */
        wr_i16(&a, 6);                     /* param size (incl itself)     */
        wr_i16(&a, 3);                     /* param code (short)           */
        wr_i16(&a, 1);                     /* param value                  */
        wr_u16(&body, (uint16_t)(a.size + 2));
        wr_put(&body, a.data, a.size);
        free(a.data);
    }

    /* group record: negative total size incl. the size field (MFA rule) */
    wr_i16(frame, (int16_t)(-(int16_t)(body.size + 2)));
    wr_put(frame, body.data, body.size);
    free(body.data);
}

static void build_frame(wr_t *frame, wr_t *item_back, wr_t *item_active)
{
    /* events payload */
    wr_t ev; wr_init(&ev);
    write_events(&ev);

    wr_i32(frame, 0);                      /* handle                       */
    wr_auto_unicode(frame, "Level 1");
    wr_i32(frame, 400);                    /* size x                       */
    wr_i32(frame, 240);                    /* size y                       */
    wr_color(frame, 32, 96, 32, 255);      /* background                   */
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
    wr_i32(frame, 2);                      /* item count                   */
    wr_put(frame, item_back->data, item_back->size);
    wr_put(frame, item_active->data, item_active->size);
    wr_i32(frame, 0);                      /* folder count                 */
    wr_i32(frame, 2);                      /* instance count               */
    /* instance: Floor at (40,20) */
    wr_i32(frame, 40);  wr_i32(frame, 20);
    wr_u32(frame, 0);   wr_i32(frame, 1);
    wr_i16(frame, 0);   wr_i16(frame, 0);
    wr_u32(frame, 0);   wr_u32(frame, 2);
    wr_i32(frame, -1);
    /* instance: Hero at (100,100) */
    wr_i32(frame, 100); wr_i32(frame, 100);
    wr_u32(frame, 0);   wr_i32(frame, 2);
    wr_i16(frame, 0);   wr_i16(frame, 0);
    wr_u32(frame, 0);   wr_u32(frame, 3);
    wr_i32(frame, -1);
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

static void build_active_item(wr_t *w)
{
    wr_i32(w, FXP_OT_ACTIVE);
    wr_i32(w, 3);                          /* handle                       */
    wr_auto_unicode(w, "Hero");
    wr_i32(w, (int32_t)0xFFFFFFFF);        /* transparent                  */
    wr_i32(w, 0);                          /* ink effect                   */
    wr_u32(w, 255);                        /* ink param                    */
    wr_i32(w, 0);                          /* antialiasing                 */
    wr_i32(w, 0);                          /* flags                        */
    wr_i32(w, 1);                          /* icon type                    */
    wr_i32(w, 0);                          /* icon handle                  */
    wr_u8(w, 0);                           /* mini-chunks terminator       */
    /* common: with one alterable value and one static movement */
    wr_i32(w, 0);                          /* object flags                 */
    wr_i32(w, 1 << 3);                     /* new flags: VisibleAtStart    */
    wr_color(w, 255, 255, 255, 255);
    for (int i = 0; i < 8; i++) wr_i16(w, -1);
    wr_i16(w, -1);
    wr_i32(w, 1);                          /* values: 1                    */
    wr_auto_unicode(w, "Health");
    wr_i32(w, FXP_VTYPE_INT);
    wr_i32(w, 42);
    wr_i32(w, 0);                          /* strings                      */
    /* movements: 1 static */
    wr_u32(w, 1);
    wr_auto_unicode(w, "Movement 1");
    wr_auto_unicode(w, "");                /* no extension                 */
    wr_u32(w, 5);                          /* identifier                   */
    wr_u32(w, 16);                         /* data size (12 + 4 pad)       */
    wr_u16(w, 0);                          /* player                       */
    wr_u16(w, 0);                          /* type: static                 */
    wr_u8(w, 1);                           /* moving at start              */
    wr_skip(w, 3);
    wr_i32(w, 0);                          /* direction at start           */
    wr_skip(w, 4);                         /* remaining movement data      */
    wr_i32(w, 0);                          /* behaviours                   */
    wr_skip(w, 2);                         /* transitions                  */
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

static void build_backdrop_item(wr_t *w)
{
    wr_i32(w, FXP_OT_BACKDROP);
    wr_i32(w, 2);                          /* handle                       */
    wr_auto_unicode(w, "Floor");
    wr_i32(w, (int32_t)0xFFFFFFFF);
    wr_i32(w, 0);
    wr_u32(w, 255);
    wr_i32(w, 0);
    wr_i32(w, 0);
    wr_i32(w, 1);                          /* icon type                    */
    wr_i32(w, 0);                          /* icon handle                  */
    wr_u8(w, 0);                           /* mini-chunks terminator       */
    wr_common(w, 1 << 3);
    /* backdrop body */
    wr_u32(w, 1);                          /* obstacle: solid              */
    wr_u32(w, 0);                          /* collision: box               */
    wr_i32(w, 7);                          /* image                        */
}

/* ------------------------------------------------------------------- main */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Fusion X-Port: synthetic MFA round-trip ==\n");

    wr_t imagebank;  wr_init(&imagebank);
    wr_i32(&imagebank, 4);                 /* AGMI graphic mode            */
    wr_i16(&imagebank, 0);                 /* palette version              */
    wr_i16(&imagebank, 0);                 /* palette entries              */
    wr_i32(&imagebank, 1);                 /* image count                  */
    build_image_item(&imagebank);

    wr_t item_back, item_active;
    wr_init(&item_back);   build_backdrop_item(&item_back);
    wr_init(&item_active); build_active_item(&item_active);

    wr_t frame; wr_init(&frame);
    build_frame(&frame, &item_back, &item_active);
    free(item_back.data);
    free(item_active.data);

    /* app */
    wr_t mfa; wr_init(&mfa);
    wr_tag(&mfa, "MFU2");
    wr_i16(&mfa, 770);
    wr_i16(&mfa, 772);
    wr_i32(&mfa, 287);
    wr_i32(&mfa, 291);
    wr_i32(&mfa, 0);
    wr_auto_unicode(&mfa, "Test Game");
    wr_auto_unicode(&mfa, "A round-trip fixture");
    wr_auto_unicode(&mfa, "C:\\Games\\Test");
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

    wr_auto_unicode(&mfa, "Test Game");    /* name (repeat)                */
    wr_auto_unicode(&mfa, "Author");
    wr_auto_unicode(&mfa, "A round-trip fixture");
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
    wr_i32(&mfa, 1);                       /* global values                */
    wr_auto_unicode(&mfa, "Score");
    wr_i32(&mfa, FXP_VTYPE_INT);
    wr_i32(&mfa, 100);
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

    /* Export the fixture for end-to-end runtime testing:
     *   FXP_DUMP_MFA=/tmp/game.mfa make -f Makefile.host test */
    {
        const char *dump = getenv("FXP_DUMP_MFA");
        if (dump) {
            FILE *fp = fopen(dump, "wb");
            if (fp) {
                fwrite(mfa.data, 1, mfa.size, fp);
                fclose(fp);
                printf("  fixture written to %s (%zu bytes)\n", dump, mfa.size);
            }
        }
    }

    /* ---- load & assert ---------------------------------------------------- */
    fxp_mfa_t *game = fxp_mfa_load(mfa.data, mfa.size);
    free(mfa.data);

    CHECK(game != NULL, "mfa loads");
    if (!game)
        return report();

    CHECK(strcmp(game->name, "Test Game") == 0, "app name");
    CHECK(game->build == 291, "build version");
    CHECK(game->frame_rate == 60, "frame rate");
    CHECK(game->initial_lives == 5, "initial lives");
    CHECK(game->frame_count == 1, "one frame");
    CHECK(game->global_values.count == 1 &&
          strcmp(game->global_values.items[0].name, "Score") == 0 &&
          game->global_values.items[0].i == 100, "global value");
    CHECK(game->control_count == 4 && game->controls[0].keys[0] == 38,
          "player controls");

    /* image bank */
    CHECK(game->images.count == 1, "one image");
    const fxp_image_t *img = &game->images.items[0];
    CHECK(img->handle == 7, "image handle");
    CHECK(img->width == 2 && img->height == 2, "image 2x2");
    CHECK(img->hotspot_x == 1 && img->hotspot_y == 2, "hotspot");
    CHECK(img->rgba != NULL, "image decoded");
    if (img->rgba) {
        CHECK(img->rgba[0] == 255 && img->rgba[1] == 0 && img->rgba[2] == 0 &&
              img->rgba[3] == 255, "pixel(0,0) red opaque");
        CHECK(img->rgba[7] == 128, "pixel(1,0) alpha from mask plane");
        CHECK(img->rgba[15] == 0, "pixel(1,1) alpha 0");
    }

    /* frame + items */
    const fxp_mfa_frame_t *f0 = game->frames[0];
    CHECK(strcmp(f0->name, "Level 1") == 0, "frame name");
    CHECK(f0->size_x == 400 && f0->size_y == 240, "frame size");
    CHECK(f0->background.r == 32, "frame background");
    CHECK(f0->item_count == 2, "two items");
    CHECK(f0->instance_count == 2, "two instances");
    CHECK(f0->events.group_count == 1, "one event group");

    fxp_item_t *active = fxp_mfa_frame_item(f0, 3);
    CHECK(active && active->object_type == FXP_OT_ACTIVE &&
          strcmp(active->name, "Hero") == 0, "active item 'Hero'");
    CHECK(active->common.values.count == 1 &&
          strcmp(active->common.values.items[0].name, "Health") == 0 &&
          active->common.values.items[0].i == 42, "alterable value");
    CHECK(active->as.active.anim_count == 1 &&
          active->as.active.anims[0].dir_count == 1 &&
          active->as.active.anims[0].dirs[0].frames[0] == 7, "animation");
    CHECK(active->common.movement_count == 1 &&
          active->common.movements[0].type == 0, "static movement");

    fxp_item_t *back = fxp_mfa_frame_item(f0, 2);
    CHECK(back && back->object_type == FXP_OT_BACKDROP &&
          back->as.backdrop.image == 7 &&
          back->as.backdrop.obstacle_type == 1, "backdrop item");

    if (f0->events.group_count == 1) {
        const fxp_event_group_t *g = &f0->events.groups[0];
        CHECK(g->condition_count == 1 && g->action_count == 1, "1 cond, 1 act");
        CHECK(g->conditions[0].object_type == FXP_OT_SYSTEM &&
              g->conditions[0].num == -1, "system 'always' condition");
        CHECK(g->actions[0].object_type == FXP_OT_SYSTEM &&
              g->actions[0].num == 4, "system action 4");
        CHECK(g->actions[0].param_count == 1 &&
              g->actions[0].params[0].code == 3 &&
              g->actions[0].params[0].int_val == 1, "short param == 1");
    }

    /* ---- runtime semantics ------------------------------------------------ */
    fxp_frame_t *rt = fxp_frame_create(game, f0);
    CHECK(fxp_frame_spawn_instances(rt) == 2, "two instances spawned");

    fxp_selector_t *sel = fxp_frame_selector(rt, 3);
    CHECK(sel && fxp_selector_count(sel) == 1, "selector for 'Hero'");

    fxp_object_t *hero = fxp_frame_find_by_info(rt, 3);
    CHECK(hero && hero->x == 100 && hero->y == 100, "hero position");

    fxp_value_t *health = fxp_object_value(hero, 0);
    fxp_value_init_int(health, 100);
    fxp_value_t other; fxp_value_init_int(&other, 58);
    fxp_value_sub(health, &other);
    fxp_value_free(&other);
    CHECK(fxp_value_as_int(health) == 42, "value 100-58 == 42");

    fxp_value_t zero; fxp_value_init_int(&zero, 0);
    fxp_value_div(health, &zero);
    fxp_value_free(&zero);
    CHECK(fxp_value_as_int(health) == 0, "divide by zero -> 0 (Fusion rule)");

    CHECK(fxp_object_current_image(hero) == 7, "hero current image 7");

    fxp_selector_iter_begin(sel);
    fxp_object_t *it = fxp_selector_iter_next(sel);
    CHECK(it == hero, "iterator yields hero");
    fxp_selector_iter_deselect_current(sel);
    CHECK(fxp_selector_iter_next(sel) == NULL, "iterator empty after deselect");
    CHECK(fxp_selector_count(sel) == 0, "selection shrunk to 0");

    fxp_selector_reset(sel, NULL);
    CHECK(fxp_selector_count(sel) == 1, "reset restores selection");

    fxp_frame_update(rt, 1.0 / 60.0);      /* must not crash               */

    fxp_frame_destroy(rt);
    fxp_mfa_free(game);

    return report();
}
