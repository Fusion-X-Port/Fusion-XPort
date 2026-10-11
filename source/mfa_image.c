/*
 * Fusion X-Port — mfa_image.c
 * MFA image pixel decoding: graphic modes 4 (24bpp + alpha plane), 6 (15bpp),
 * 7 (16bpp), 8 (32bpp).  Padding rules ported from CTFAK's ImageTranslator
 * (GetPadding / Normal24BitMaskedToRGBA / RGBAToRGBMasked).
 */
#include "mfa_internal.h"
#include "fxp/mfa_format.h"

#include <zlib.h>

/* Inflate a raw zlib stream (CTFAK Decompressor.DecompressBlock). */
uint8_t *fxp_zlib_inflate(const uint8_t *src, size_t src_size,
                          size_t expected, size_t *out_size)
{
    size_t cap = expected ? expected : (src_size * 20 + 4096);
    for (int attempt = 0; attempt < 4; attempt++) {
        uint8_t *dst = fxp_malloc(cap);
        uLongf  dlen = (uLongf)cap;
        int rc = uncompress(dst, &dlen, src, (uLong)src_size);
        if (rc == Z_OK) {
            if (out_size) *out_size = dlen;
            return dst;
        }
        free(dst);
        if (rc != Z_BUF_ERROR || expected)
            break;
        cap *= 4;
    }
    fxp_log_error("zlib inflate failed (%zu -> ?)\n", src_size);
    return NULL;
}

void fxp_image_free_rgba(fxp_image_t *img)
{
    free(img->rgba);
    img->rgba = NULL;
    img->rgba_size = 0;
    free(img->col_mask);
    img->col_mask = NULL;
    img->col_mask_stride = 0;
}

/*
 * 1bpp collision mask built from the decoded RGBA (NuclearRT
 * CollisionMaskBuilder): bit set when alpha > 128, MSB first
 * (bit = 0x80 >> (x & 7)), row stride (width + 7) / 8, no extra padding.
 * Fusion stores no masks in the MFA image bank — they are derived from
 * the sprites at runtime (NuclearRT GetCollisionMaskData equivalent).
 */
void fxp_image_build_collision_mask(fxp_image_t *img)
{
    free(img->col_mask);
    img->col_mask = NULL;
    img->col_mask_stride = 0;
    if (!img->rgba || img->width <= 0 || img->height <= 0)
        return;
    uint32_t stride = (uint32_t)((img->width + 7) / 8);
    uint8_t *mask = fxp_calloc((size_t)stride * (size_t)img->height, 1);
    for (int32_t y = 0; y < img->height; y++) {
        const uint8_t *row = img->rgba + (size_t)y * (size_t)img->width * 4;
        uint8_t *mrow = mask + (size_t)y * stride;
        for (int32_t x = 0; x < img->width; x++) {
            if (row[(size_t)x * 4 + 3] > 128)
                mrow[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
    img->col_mask = mask;
    img->col_mask_stride = stride;
}

static inline uint8_t scale5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t scale6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

static inline bool img_has_alpha(const fxp_image_t *img)
{
    return (img->flags & FXP_IMGFLAG_ALPHA) != 0;
}

/* When the alpha plane is present it FULLY replaces the transparent-color
 * rule (CTFAK's alpha-plane loop overwrites every alpha byte). */
static inline uint8_t alpha_pick(const fxp_image_t *img, uint8_t r, uint8_t g,
                                 uint8_t b, bool has_alpha, uint8_t plane)
{
    if (has_alpha)
        return plane;
    return (r == img->transparent.r && g == img->transparent.g &&
            b == img->transparent.b) ? 0 : 255;
}

/*
 * Graphic mode 4: the MFA stores rows as B,G,R — NOT R,G,B.
 * Verified against CTFAK's own MFA writer (ImageTranslator.FromBitmap,
 * which memcpy's GDI Format24bppRgb — BGR in memory — straight into the
 * image stream) and against real Fusion-authored files: decoding as RGB
 * swaps red/blue (green sprites render olive/"yellowed", peach skin
 * renders lavender).  Row padding, in pixel units (CTFAK
 * GetPadding(width, 3, bytes=2)): odd widths advance one extra 3-byte pixel.
 * Alpha plane (FXP_IMGFLAG_ALPHA): one byte per pixel, rows padded to
 * 4-byte boundaries (GetPadding(width, 1, bytes=4)).
 */
static bool decode_mode4(const uint8_t *src, size_t src_size, fxp_image_t *img)
{
    const int32_t w = img->width, h = img->height;
    if (w < 0 || h < 0) return false;
    size_t rgb_stride   = (size_t)w * 3 + ((w & 1) ? 3 : 0);
    size_t alpha_stride = (size_t)w + ((4 - ((size_t)w % 4)) % 4);
    bool has_alpha = img_has_alpha(img);

    size_t need = rgb_stride * (size_t)h + (has_alpha ? alpha_stride * (size_t)h : 0);
    if (src_size < need) {
        fxp_log_error("image %d mode 4: %zu bytes < needed %zu\n",
                      img->handle, src_size, need);
        return false;
    }

    img->rgba_size = (size_t)w * (size_t)h * 4;
    img->rgba = fxp_malloc(img->rgba_size);

    const uint8_t *rgb  = src;
    const uint8_t *mask = src + rgb_stride * (size_t)h;

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row  = rgb  + rgb_stride   * (size_t)y;
        const uint8_t *arow = mask + alpha_stride * (size_t)y;
        for (int32_t x = 0; x < w; x++) {
            uint8_t *out = img->rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = alpha_pick(img, r, g, b, has_alpha, has_alpha ? arow[x] : 0);
        }
    }
    return true;
}

/* Graphic mode 6: 15bpp RGB555, packed, no row padding. */
static bool decode_mode6(const uint8_t *src, size_t src_size, fxp_image_t *img)
{
    const int32_t w = img->width, h = img->height;
    size_t need = (size_t)w * (size_t)h * 2;
    if (w < 0 || h < 0 || src_size < need) return false;

    img->rgba_size = need * 2;
    img->rgba = fxp_malloc(img->rgba_size);
    for (int32_t i = 0; i < w * h; i++) {
        uint16_t s = (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
        uint8_t r = scale5((s >> 10) & 0x1F);
        uint8_t g = scale5((s >> 5) & 0x1F);
        uint8_t b = scale5(s & 0x1F);
        uint8_t *out = img->rgba + (size_t)i * 4;
        out[0] = r; out[1] = g; out[2] = b;
        out[3] = alpha_pick(img, r, g, b, img_has_alpha(img), 0);
    }
    return true;
}

/* Graphic mode 7: 16bpp RGB565, packed, no row padding. */
static bool decode_mode7(const uint8_t *src, size_t src_size, fxp_image_t *img)
{
    const int32_t w = img->width, h = img->height;
    size_t need = (size_t)w * (size_t)h * 2;
    if (w < 0 || h < 0 || src_size < need) return false;

    img->rgba_size = need * 2;
    img->rgba = fxp_malloc(img->rgba_size);
    for (int32_t i = 0; i < w * h; i++) {
        uint16_t s = (uint16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
        uint8_t r = scale5((s >> 11) & 0x1F);
        uint8_t g = scale6((s >> 5) & 0x3F);
        uint8_t b = scale5(s & 0x1F);
        uint8_t *out = img->rgba + (size_t)i * 4;
        out[0] = r; out[1] = g; out[2] = b;
        out[3] = alpha_pick(img, r, g, b, img_has_alpha(img), 0);
    }
    return true;
}

/* Graphic mode 8: 32bpp RGBA, no padding. */
static bool decode_mode8(const uint8_t *src, size_t src_size, fxp_image_t *img)
{
    const int32_t w = img->width, h = img->height;
    size_t need = (size_t)w * (size_t)h * 4;
    if (w < 0 || h < 0 || src_size < need) return false;

    img->rgba_size = need;
    img->rgba = fxp_malloc(need);
    memcpy(img->rgba, src, need);
    return true;
}

bool fxp_image_decode_pixels(fxp_image_t *img, const uint8_t *raw, size_t raw_size)
{
    switch (img->graphic_mode) {
    case FXP_GM_24BIT: return decode_mode4(raw, raw_size, img);
    case FXP_GM_15BIT: return decode_mode6(raw, raw_size, img);
    case FXP_GM_16BIT: return decode_mode7(raw, raw_size, img);
    case FXP_GM_32BIT: return decode_mode8(raw, raw_size, img);
    default:
        fxp_log_warn("image %d: unsupported graphic mode %d — opaque fill\n",
                     img->handle, img->graphic_mode);
        img->rgba_size = (size_t)(img->width > 0 ? img->width : 1) *
                         (size_t)(img->height > 0 ? img->height : 1) * 4;
        img->rgba = fxp_calloc(1, img->rgba_size);
        return true;
    }
}
