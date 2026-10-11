/*
 * Fusion X-Port — byte_reader.c
 */
#include "fxp/byte_reader.h"

#include <stdarg.h>
#include <ctype.h>

/* ------------------------------------------------------------------ log -- */

static void fxp_vlog(const char *level, const char *fmt, va_list ap)
{
    fputs(level, stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void fxp_log_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fxp_vlog("[fxp] ", fmt, ap);
    va_end(ap);
}

void fxp_log_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fxp_vlog("[fxp][warn] ", fmt, ap);
    va_end(ap);
}

void fxp_log_error(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fxp_vlog("[fxp][error] ", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ mem -- */

void *fxp_malloc(size_t size)
{
    void *p = malloc(size);
    if (!p && size) {
        fxp_log_error("out of memory (%zu bytes)\n", size);
        abort();
    }
    return p;
}

void *fxp_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (!p && n && size) {
        fxp_log_error("out of memory (%zu x %zu bytes)\n", n, size);
        abort();
    }
    return p;
}

void *fxp_realloc(void *p, size_t size)
{
    void *q = realloc(p, size);
    if (!q && size) {
        fxp_log_error("out of memory (%zu bytes)\n", size);
        abort();
    }
    return q;
}

char *fxp_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = fxp_malloc(n);
    memcpy(d, s, n);
    return d;
}

/* ---------------------------------------------------------------- reader -- */

void fxp_r_init(fxp_reader_t *r, const void *data, size_t size)
{
    r->base = (const uint8_t *)data;
    r->size = size;
    r->pos  = 0;
}

bool fxp_r_ok(const fxp_reader_t *r)
{
    return r->pos <= r->size;
}

size_t fxp_r_tell(const fxp_reader_t *r) { return r->pos; }
size_t fxp_r_size(const fxp_reader_t *r) { return r->size; }
const uint8_t *fxp_r_ptr(const fxp_reader_t *r) { return r->base + r->pos; }

bool fxp_r_seek(fxp_reader_t *r, size_t pos)
{
    if (pos > r->size) {
        fxp_log_error("reader seek %zu beyond %zu\n", pos, r->size);
        r->pos = r->size;
        return false;
    }
    r->pos = pos;
    return true;
}

bool fxp_r_skip(fxp_reader_t *r, size_t n)
{
    if (r->pos + n > r->size) {
        fxp_log_error("reader skip %zu at %zu beyond %zu\n", n, r->pos, r->size);
        r->pos = r->size;
        return false;
    }
    r->pos += n;
    return true;
}

int32_t fxp_r_peek_i32(const fxp_reader_t *r)
{
    if (r->pos + 4 > r->size)
        return 0;
    const uint8_t *p = r->base + r->pos;
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

bool fxp_r_read(fxp_reader_t *r, void *out, size_t n)
{
    if (r->pos + n > r->size) {
        fxp_log_error("reader read %zu at %zu beyond %zu\n", n, r->pos, r->size);
        r->pos = r->size;
        return false;
    }
    memcpy(out, r->base + r->pos, n);
    r->pos += n;
    return true;
}

uint8_t fxp_r_u8(fxp_reader_t *r)
{
    uint8_t v = 0;
    fxp_r_read(r, &v, 1);
    return v;
}

int8_t fxp_r_i8(fxp_reader_t *r) { return (int8_t)fxp_r_u8(r); }

uint16_t fxp_r_u16(fxp_reader_t *r)
{
    uint8_t b[2] = {0, 0};
    fxp_r_read(r, b, 2);
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

int16_t fxp_r_i16(fxp_reader_t *r) { return (int16_t)fxp_r_u16(r); }

uint32_t fxp_r_u32(fxp_reader_t *r)
{
    uint8_t b[4] = {0, 0, 0, 0};
    fxp_r_read(r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int32_t fxp_r_i32(fxp_reader_t *r) { return (int32_t)fxp_r_u32(r); }

float fxp_r_f32(fxp_reader_t *r)
{
    uint32_t u = fxp_r_u32(r);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

double fxp_r_f64(fxp_reader_t *r)
{
    uint8_t b[8] = {0};
    fxp_r_read(r, b, 8);
    uint64_t u = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
                 ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32) |
                 ((uint64_t)b[5] << 40) | ((uint64_t)b[6] << 48) |
                 ((uint64_t)b[7] << 56);
    double d;
    memcpy(&d, &u, sizeof(d));
    return d;
}

bool fxp_r_ascii4(fxp_reader_t *r, char *out)
{
    if (!fxp_r_read(r, out, 4))
        return false;
    out[4] = '\0';
    return true;
}

fxp_color_t fxp_r_color(fxp_reader_t *r)
{
    fxp_color_t c;
    c.r = fxp_r_u8(r);
    c.g = fxp_r_u8(r);
    c.b = fxp_r_u8(r);
    c.a = fxp_r_u8(r);
    return c;
}

/* UTF-16LE code unit -> UTF-8 (with surrogate pairs).  Returns bytes written
 * to `out` (which must be able to hold up to 4 bytes). */
static int fxp_utf16_unit_to_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

char *fxp_r_utf16(fxp_reader_t *r, int32_t codeunits)
{
    if (codeunits < 0 || (size_t)codeunits * 2 > r->size - r->pos) {
        fxp_log_error("utf16 string of %d units overruns reader\n", codeunits);
        return fxp_strdup("");
    }

    /* worst case: 3 bytes per BMP unit + safe slack */
    size_t cap = (size_t)codeunits * 4 + 1;
    char  *out = fxp_malloc(cap);
    size_t len = 0;

    for (int32_t i = 0; i < codeunits; i++) {
        uint16_t u = fxp_r_u16(r);
        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < codeunits) {
            uint16_t lo = fxp_r_u16(r);
            i++;
            if (lo >= 0xDC00 && lo <= 0xDFFF)
                cp = 0x10000 + (((uint32_t)(u - 0xD800)) << 10) + (lo - 0xDC00);
        }
        len += (size_t)fxp_utf16_unit_to_utf8(cp, out + len);
    }
    out[len] = '\0';
    return out;
}

char *fxp_r_auto_unicode(fxp_reader_t *r)
{
    int16_t len   = fxp_r_i16(r);
    int16_t check = fxp_r_i16(r);
    if (check != (int16_t)0x8000) {
        fxp_log_error("auto-unicode marker 0x%04x != 0x8000 (len %d) "
                      "at offset %zu\n",
                      (unsigned)check, len, fxp_r_tell(r) - 4);
        return fxp_strdup("");
    }
    return fxp_r_utf16(r, len);
}

char *fxp_r_yuniversal(fxp_reader_t *r, int32_t codeunits)
{
    int32_t len = codeunits;
    if (len < 0)
        len = fxp_r_i16(r);
    return fxp_r_utf16(r, len);
}
