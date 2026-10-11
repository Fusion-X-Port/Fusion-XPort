/*
 * Fusion X-Port — backend.h
 * Platform abstraction over SDL3: window/render surface, input, audio and
 * timing.  The same interface will later be implementable per console.
 */
#ifndef FXP_BACKEND_H
#define FXP_BACKEND_H

#include "fxp.h"
#include "byte_reader.h"          /* fxp_color_t */
#include "mfa.h"                  /* fxp_image (forward use) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fxp_render fxp_render_t;
typedef struct fxp_audio fxp_audio_t;

fxp_render_t *fxp_render_create(void);
void          fxp_render_destroy(fxp_render_t *r);
/* Drop every uploaded texture (image handles are per-MFA). */
void          fxp_render_clear_images(fxp_render_t *r);
/* Upload/refresh the RGBA image with the given bank handle. */
bool          fxp_render_upload_image(fxp_render_t *r, const struct fxp_image *img);
void          fxp_render_begin(fxp_render_t *r, fxp_color_t clear);
void          fxp_render_blit(fxp_render_t *r, int32_t image_handle,
                              int32_t x, int32_t y,
                              int32_t hotspot_x, int32_t hotspot_y,
                              uint32_t angle, uint8_t blend);
/* Blit with per-texture RGB tint (Fusion RGB coefficient). */
void          fxp_render_blit_tinted(fxp_render_t *r, int32_t image_handle,
                                     int32_t x, int32_t y,
                                     int32_t hotspot_x, int32_t hotspot_y,
                                     uint32_t angle, uint8_t blend,
                                     uint8_t tint_r, uint8_t tint_g,
                                     uint8_t tint_b);
void          fxp_render_fill_rect(fxp_render_t *r, int32_t x, int32_t y,
                                   int32_t w, int32_t h, fxp_color_t color);
void          fxp_render_end(fxp_render_t *r);
/* Raw SDL renderer — for the launcher UI (SDL_RenderDebugText). */
struct SDL_Renderer;
struct SDL_Renderer *fxp_render_sdl(fxp_render_t *r);
/* Read back the presented frame and write a binary PPM (debug/CI). */
bool          fxp_render_snapshot(fxp_render_t *r, const char *path);

fxp_audio_t  *fxp_audio_create(void);
void          fxp_audio_destroy(fxp_audio_t *a);
/* `data`/`size` must be a whole WAV/OGG/MP3 blob; returns channel index
 * or -1.  Scaffold decodes WAV via SDL, other formats are logged. */
int           fxp_audio_load(fxp_audio_t *a, const uint8_t *data, size_t size);
void          fxp_audio_play(fxp_audio_t *a, int channel, int loops);
void          fxp_audio_stop_all(fxp_audio_t *a);

#ifdef __cplusplus
}
#endif
#endif /* FXP_BACKEND_H */
