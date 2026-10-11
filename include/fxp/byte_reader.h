/*
 * Fusion X-Port — byte_reader.h
 * Little-endian, bounds-checked binary reader for MFA/CCN data.
 *
 * Mirrors the conventions documented by CTFAK 2.0 (Core/CTFAK.Core/Memory/ByteReader.cs):
 *   - all integers little-endian
 *   - colors stored as R,G,B,A byte quadruplets
 *   - "AutoUnicode" strings: int16 length, int16 marker (must be 0x8000),
 *     then `length` UTF-16LE code units
 *   - "Yuniversal" strings: int16 length (if not supplied), then UTF-16LE
 */
#ifndef FXP_BYTE_READER_H
#define FXP_BYTE_READER_H

#include "fxp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxp_color {
    uint8_t r, g, b, a;
} fxp_color_t;

typedef struct fxp_reader {
    const uint8_t *base;   /* start of buffer                       */
    size_t         size;   /* total buffer size                     */
    size_t         pos;    /* current read offset                   */
} fxp_reader_t;

void   fxp_r_init(fxp_reader_t *r, const void *data, size_t size);
bool   fxp_r_ok(const fxp_reader_t *r);
size_t fxp_r_tell(const fxp_reader_t *r);
size_t fxp_r_size(const fxp_reader_t *r);
bool   fxp_r_seek(fxp_reader_t *r, size_t pos);
bool   fxp_r_skip(fxp_reader_t *r, size_t n);
const uint8_t *fxp_r_ptr(const fxp_reader_t *r);
int32_t fxp_r_peek_i32(const fxp_reader_t *r);

uint8_t  fxp_r_u8(fxp_reader_t *r);
int8_t   fxp_r_i8(fxp_reader_t *r);
uint16_t fxp_r_u16(fxp_reader_t *r);
int16_t  fxp_r_i16(fxp_reader_t *r);
uint32_t fxp_r_u32(fxp_reader_t *r);
int32_t  fxp_r_i32(fxp_reader_t *r);
float    fxp_r_f32(fxp_reader_t *r);
double   fxp_r_f64(fxp_reader_t *r);

/* Point read into caller buffer; returns false on out-of-bounds. */
bool fxp_r_read(fxp_reader_t *r, void *out, size_t n);

/* Four-byte tag ("ATNF", "MFU2", ...). out must hold at least 5 bytes. */
bool fxp_r_ascii4(fxp_reader_t *r, char *out);

/* R,G,B,A quadruplet. */
fxp_color_t fxp_r_color(fxp_reader_t *r);

/* Read `n` UTF-16LE code units, convert to UTF-8 (owned, free()). */
char *fxp_r_utf16(fxp_reader_t *r, int32_t codeunits);

/* AutoUnicode: i16 len + i16 0x8000 marker + len UTF-16LE code units. */
char *fxp_r_auto_unicode(fxp_reader_t *r);

/* Yuniversal: optional i16 len prefix then UTF-16LE code units. */
char *fxp_r_yuniversal(fxp_reader_t *r, int32_t codeunits);

#ifdef __cplusplus
}
#endif
#endif /* FXP_BYTE_READER_H */
