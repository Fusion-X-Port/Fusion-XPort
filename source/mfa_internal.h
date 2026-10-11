/*
 * Fusion X-Port — mfa_internal.h (private, not installed)
 * Shared helpers between the MFA reader translation units.
 */
#ifndef FXP_MFA_INTERNAL_H
#define FXP_MFA_INTERNAL_H

#include "fxp/mfa.h"

/* MFA mini-chunk stream: repeated { u8 id, i32 size, bytes } terminated by
 * id 0 (MFAChunks.Read).  Caller frees via fxp_free_chunks. */
typedef struct fxp_chunk_list {
    int32_t          count;
    fxp_mfa_chunk_t *items;
} fxp_chunk_list_t;

bool fxp_chunks_read(fxp_reader_t *r, fxp_chunk_list_t *out);
void fxp_chunks_free(fxp_chunk_list_t *list);
void fxp_chunks_free_ptr(int32_t count, fxp_mfa_chunk_t *items);

/* Inflate a raw zlib stream (mfa_image.c). */
uint8_t *fxp_zlib_inflate(const uint8_t *src, size_t src_size,
                          size_t expected, size_t *out_size);

/* Decode raw pixel data into img->rgba (mfa_image.c). */
bool fxp_image_decode_pixels(fxp_image_t *img, const uint8_t *raw, size_t raw_size);

/* Build the 1bpp collision mask from the decoded alpha channel
 * (alpha > 128, MSB first, stride (width+7)/8). */
void fxp_image_build_collision_mask(fxp_image_t *img);

/* Free one image's decoded pixels + collision mask. */
void fxp_image_free_rgba(fxp_image_t *img);

/* Events reader (mfa_events.c). */
bool fxp_events_read(fxp_reader_t *r, fxp_events_t *out);
void fxp_events_free(fxp_events_t *ev);

/* Value list reader (mfa_objects.c). */
bool fxp_value_list_read(fxp_reader_t *r, fxp_value_list_t *out);
void fxp_value_list_free(fxp_value_list_t *list);

/* Parse per-type movement loader params (mfa_objects.c). */
void fxp_movement_parse_params(fxp_movement_t *m);

/* Frame item (object info) reader (mfa_objects.c). */
bool fxp_item_read(fxp_reader_t *r, fxp_item_t **out);
void fxp_item_free(fxp_item_t *item);

#endif /* FXP_MFA_INTERNAL_H */
