/*
 * Fusion X-Port — render_sdl3.c
 * SDL3 renderer backend.  On the 3DS SDL3 selects its framebuffer software
 * driver automatically (SDL_VIDEO_DRIVER_N3DS); SDL_Renderer falls back to
 * the software renderer there, matching SDL's official N3DS notes.
 */
#include "fxp/backend.h"

#include <SDL3/SDL.h>

typedef struct tex_entry {
    int32_t       handle;
    SDL_Texture  *tex;
} tex_entry_t;

struct fxp_render {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    tex_entry_t  *textures;
    int32_t       texture_count;
    int32_t       texture_cap;
    int32_t       uploaded_count;    /* how many entries are valid      */
};

fxp_render_t *fxp_render_create(void)
{
    fxp_render_t *r = fxp_calloc(1, sizeof(fxp_render_t));

    /* 3DS top screen is 400x240; scale 2x on desktop for readability. */
    int win_w = FXP_SCREEN_W, win_h = FXP_SCREEN_H;
#if !defined(FXP_PLATFORM_3DS) && !defined(FXP_PLATFORM_SWITCH)
    win_w *= 2;
    win_h *= 2;
#endif
    if (!SDL_CreateWindowAndRenderer("Fusion X-Port", win_w, win_h,
                                     SDL_WINDOW_RESIZABLE,
                                     &r->window, &r->renderer)) {
        fxp_log_error("render: %s", SDL_GetError());
        free(r);
        return NULL;
    }
    SDL_SetRenderLogicalPresentation(r->renderer, FXP_SCREEN_W, FXP_SCREEN_H,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    return r;
}

void fxp_render_destroy(fxp_render_t *r)
{
    if (!r)
        return;
    for (int32_t i = 0; i < r->uploaded_count; i++)
        SDL_DestroyTexture(r->textures[i].tex);
    free(r->textures);
    SDL_DestroyRenderer(r->renderer);
    SDL_DestroyWindow(r->window);
    free(r);
}

void fxp_render_clear_images(fxp_render_t *r)
{
    if (!r)
        return;
    for (int32_t i = 0; i < r->uploaded_count; i++)
        SDL_DestroyTexture(r->textures[i].tex);
    r->uploaded_count = 0;
}

static SDL_Texture *lookup(fxp_render_t *r, int32_t handle)
{
    for (int32_t i = 0; i < r->uploaded_count; i++)
        if (r->textures[i].handle == handle)
            return r->textures[i].tex;
    return NULL;
}

bool fxp_render_upload_image(fxp_render_t *r, const fxp_image_t *img)
{
    if (!img || !img->rgba)
        return false;
    if (lookup(r, img->handle))
        return true;                      /* already resident             */

    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        img->width, img->height, SDL_PIXELFORMAT_ABGR8888,
        img->rgba, img->width * 4);
    if (!surface) {
        fxp_log_error("render: surface for image %d: %s", img->handle, SDL_GetError());
        return false;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r->renderer, surface);
    SDL_DestroySurface(surface);
    if (!tex) {
        fxp_log_error("render: texture for image %d: %s", img->handle, SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    /* SDL3 defaults textures to SDL_BLENDMODE_NONE — without BLEND the
     * transparent pixels paint their RGB instead of compositing. */
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    if (r->uploaded_count == r->texture_cap) {
        r->texture_cap = r->texture_cap ? r->texture_cap * 2 : 16;
        r->textures = fxp_realloc(r->textures,
                                  (size_t)r->texture_cap * sizeof(tex_entry_t));
    }
    r->textures[r->uploaded_count].handle = img->handle;
    r->textures[r->uploaded_count].tex = tex;
    r->uploaded_count++;
    return true;
}

void fxp_render_begin(fxp_render_t *r, fxp_color_t clear)
{
    SDL_SetRenderDrawColor(r->renderer, clear.r, clear.g, clear.b, clear.a);
    SDL_RenderClear(r->renderer);
}

void fxp_render_blit(fxp_render_t *r, int32_t image_handle,
                     int32_t x, int32_t y,
                     int32_t hotspot_x, int32_t hotspot_y,
                     uint32_t angle, uint8_t blend)
{
    fxp_render_blit_tinted(r, image_handle, x, y, hotspot_x, hotspot_y,
                           angle, blend, 255, 255, 255);
}

void fxp_render_blit_tinted(fxp_render_t *r, int32_t image_handle,
                            int32_t x, int32_t y,
                            int32_t hotspot_x, int32_t hotspot_y,
                            uint32_t angle, uint8_t blend,
                            uint8_t tint_r, uint8_t tint_g, uint8_t tint_b)
{
    SDL_Texture *tex = lookup(r, image_handle);
    if (!tex)
        return;
    float w, h;
    SDL_GetTextureSize(tex, &w, &h);
    SDL_FRect dst = { (float)(x - hotspot_x), (float)(y - hotspot_y), w, h };
    SDL_SetTextureAlphaMod(tex, blend);
    SDL_SetTextureColorMod(tex, tint_r, tint_g, tint_b);
    if (angle != 0) {
        SDL_RenderTextureRotated(r->renderer, tex, NULL, &dst,
                                 (double)angle, NULL, SDL_FLIP_NONE);
    } else {
        SDL_RenderTexture(r->renderer, tex, NULL, &dst);
    }
}

void fxp_render_fill_rect(fxp_render_t *r, int32_t x, int32_t y,
                          int32_t w, int32_t h, fxp_color_t color)
{
    SDL_SetRenderDrawColor(r->renderer, color.r, color.g, color.b, color.a);
    SDL_FRect rect = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r->renderer, &rect);
}

void fxp_render_end(fxp_render_t *r)
{
    SDL_RenderPresent(r->renderer);
}

SDL_Renderer *fxp_render_sdl(fxp_render_t *r)
{
    return r ? r->renderer : NULL;
}

bool fxp_render_snapshot(fxp_render_t *r, const char *path)
{
    if (!r || !path)
        return false;
    SDL_Surface *surf = SDL_RenderReadPixels(r->renderer, NULL);
    if (!surf)
        return false;
    FILE *f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", surf->w, surf->h);
        const SDL_PixelFormatDetails *fmt = SDL_GetPixelFormatDetails(surf->format);
        const uint8_t *px = (const uint8_t *)surf->pixels;
        int bpp = fmt ? fmt->bytes_per_pixel : 4;
        for (int y = 0; y < surf->h; y++)
            for (int x = 0; x < surf->w; x++) {
                const uint8_t *p = px + y * surf->pitch + x * bpp;
                uint32_t pixel = 0;
                memcpy(&pixel, p, (size_t)(bpp > 4 ? 4 : bpp));
                uint8_t rgb[3];
                SDL_GetRGB(pixel, fmt, NULL, &rgb[0], &rgb[1], &rgb[2]);
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        ok = true;
    }
    SDL_DestroySurface(surf);
    return ok;
}
