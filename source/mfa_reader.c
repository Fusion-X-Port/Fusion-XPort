/*
 * Fusion X-Port — mfa_reader.c
 * The MFA loader: header, banks, frames (per CTFAK's MFAData.Read).
 *
 * Fusion X-Port deliberately supports only the plain editor format (MFA):
 * there is no EXE/CCN unpacking and no decryption here, so the runtime
 * never touches compiled distributions.
 */
#include "mfa_internal.h"
#include "fxp/mfa_format.h"

/* ---------------------------------------------------------------- chunks -- */

bool fxp_chunks_read(fxp_reader_t *r, fxp_chunk_list_t *out)
{
    memset(out, 0, sizeof(*out));
    int32_t cap = 8;
    out->items = fxp_malloc((size_t)cap * sizeof(fxp_mfa_chunk_t));

    for (;;) {
        uint8_t id = fxp_r_u8(r);
        if (!fxp_r_ok(r))
            goto fail;
        if (id == 0)
            break;                        /* terminator (MFAChunk.Read)    */
        int32_t size = fxp_r_i32(r);
        if (size < 0 || fxp_r_tell(r) + (size_t)size > fxp_r_size(r)) {
            fxp_log_error("chunk %u: bad size %d\n", id, size);
            goto fail;
        }
        if (out->count == cap) {
            cap *= 2;
            out->items = fxp_realloc(out->items, (size_t)cap * sizeof(fxp_mfa_chunk_t));
        }
        fxp_mfa_chunk_t *c = &out->items[out->count++];
        c->id   = id;
        c->size = size;
        c->data = NULL;
        if (size > 0) {
            c->data = fxp_malloc((size_t)size);
            if (!fxp_r_read(r, c->data, (size_t)size))
                goto fail;
        }
    }
    return true;

fail:
    fxp_chunks_free(out);
    return false;
}

void fxp_chunks_free_ptr(int32_t count, fxp_mfa_chunk_t *items)
{
    for (int32_t i = 0; i < count; i++)
        free(items[i].data);
    free(items);
}

void fxp_chunks_free(fxp_chunk_list_t *list)
{
    fxp_chunks_free_ptr(list->count, list->items);
    list->items = NULL;
    list->count = 0;
}

/* ----------------------------------------------------------------- images -- */

/* Read one AGMI image item (FusionImage MFA path, verified against
 * FusionImage.Read lines 254-358 + WriteNew lines 360-389). */
static bool image_item_read(fxp_reader_t *r, fxp_image_t *img)
{
    memset(img, 0, sizeof(*img));
    img->handle    = fxp_r_i32(r);       /* NO -1 adjustment for MFA        */
    img->checksum  = fxp_r_i32(r);
    img->references = fxp_r_i32(r);
    int32_t inner  = fxp_r_i32(r);       /* compressed length + 4           */

    if (inner < 4)
        return false;

    /* The remainder of the item is a local block of inner+20 bytes whose
     * head duplicates nothing — CTFAK re-reads fields from a sub-reader
     * over (dataSize + 20) bytes. */
    size_t local_size = (size_t)inner + 20;
    if (fxp_r_tell(r) + local_size > fxp_r_size(r)) {
        fxp_log_error("image %d: item block overruns file\n", img->handle);
        return false;
    }
    fxp_reader_t local;
    {
        const uint8_t *p = fxp_r_ptr(r);
        fxp_r_init(&local, p, local_size);
        fxp_r_skip(r, local_size);
    }

    img->width  = fxp_r_i16(&local);
    img->height = fxp_r_i16(&local);
    img->graphic_mode = fxp_r_u8(&local);
    img->flags  = fxp_r_u8(&local);
    (void)fxp_r_i16(&local);             /* reserved                        */
    img->hotspot_x = fxp_r_i16(&local);
    img->hotspot_y = fxp_r_i16(&local);
    img->action_x  = fxp_r_i16(&local);
    img->action_y  = fxp_r_i16(&local);
    img->transparent = fxp_r_color(&local);

    int32_t decomp_size = fxp_r_i32(&local);
    size_t compressed = local_size - 24;
    if (decomp_size < 0) {
        fxp_log_error("image %d: negative decomp size\n", img->handle);
        return false;
    }

    if (img->flags & FXP_IMGFLAG_LZX) {
        size_t raw_size = 0;
        uint8_t *raw = fxp_zlib_inflate(fxp_r_ptr(&local), compressed,
                                        (size_t)decomp_size, &raw_size);
        if (!raw)
            return false;
        bool ok = fxp_image_decode_pixels(img, raw, raw_size);
        free(raw);
        if (ok)
            fxp_image_build_collision_mask(img);
        return ok;
    }
    bool ok = fxp_image_decode_pixels(img, fxp_r_ptr(&local), compressed);
    if (ok)
        fxp_image_build_collision_mask(img);
    return ok;
}

static bool image_bank_read(fxp_reader_t *r, fxp_image_bank_t *bank)
{
    memset(bank, 0, sizeof(*bank));
    bank->graphic_mode    = fxp_r_i32(r);
    bank->palette_version = fxp_r_i16(r);
    bank->palette_entries = fxp_r_i16(r);
    for (int32_t i = 0; i < bank->palette_entries && i < 256; i++)
        bank->palette[i] = fxp_r_color(r);

    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 262144) {
        fxp_log_error("image bank: absurd count %d\n", count);
        return false;
    }
    bank->count = count;
    bank->items = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_image_t));
    for (int32_t i = 0; i < count; i++) {
        if (!image_item_read(r, &bank->items[i])) {
            fxp_log_error("image bank: item %d/%d failed\n", i + 1, count);
            return false;
        }
    }
    return fxp_r_ok(r);
}

/* ----------------------------------------------------------------- sounds -- */

static bool sound_bank_read(fxp_reader_t *r, fxp_sound_bank_t *bank)
{
    /* MFA: IsCompressed = false (MFAData.cs:322).  SoundItem.Read:
     * u32 handle, i32 checksum, u32 refs, i32 decompSize, u8 flags,
     * skip 3, i32 reserved, i32 nameLen, name+data.
     *
     * The handle is stored RAW — for the first sound Fusion writes 0 and
     * the Play-sample event param also references 0 (verified against
     * real build-295 files).  The old "- 1" here turned the first item
     * into handle -1 so the event lookup never matched and every game
     * was silent.  CTFAK's SoundItem.Read subtracts 1 on read and adds
     * nothing back on write, so CTFAK-round-tripped files land in the
     * same 0-based space; anything else is covered by the position
     * fallback in the sample lookup (event_runtime.c). */
    memset(bank, 0, sizeof(*bank));
    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 262144)
        return false;
    bank->count = count;
    bank->items = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_sound_t));

    for (int32_t i = 0; i < count; i++) {
        fxp_sound_t *s = &bank->items[i];
        s->handle    = (int32_t)fxp_r_u32(r);
        s->checksum  = fxp_r_i32(r);
        s->references = fxp_r_u32(r);
        int32_t decomp = fxp_r_i32(r);
        s->flags = fxp_r_u8(r);
        fxp_r_skip(r, 3);
        (void)fxp_r_i32(r);              /* reserved                        */
        int32_t name_len = fxp_r_i32(r);
        if (decomp < 0 || name_len < 0 ||
            fxp_r_tell(r) + (size_t)decomp > fxp_r_size(r)) {
            fxp_log_error("sound %d: bad sizes\n", s->handle);
            return false;
        }
        s->data_size = decomp;
        uint8_t *blob = fxp_malloc((size_t)(decomp ? decomp : 1));
        if (decomp && !fxp_r_read(r, blob, (size_t)decomp)) {
            free(blob);
            return false;
        }
        /* name = first name_len UTF-16 units of the blob, data = the rest */
        fxp_reader_t sr;
        fxp_r_init(&sr, blob, (size_t)decomp);
        s->name = fxp_r_utf16(&sr, name_len);
        size_t rest = decomp - (size_t)name_len * 2;
        s->data = fxp_malloc(rest ? rest : 1);
        memcpy(s->data, blob + (size_t)name_len * 2, rest);
        s->data_size = (int32_t)rest;
        free(blob);
    }
    return fxp_r_ok(r);
}

/* ------------------------------------------------------------------ fonts -- */

static bool font_bank_read(fxp_reader_t *r, fxp_font_bank_t *bank, int32_t build)
{
    /* MFA: uncompressed (MFAData.cs:317).  FontItem.Read:
     * u32 handle (+ -1 offset when build > 284), i32 checksum, i32 refs,
     * i32 size, LogFont{ 5xi32, 8xu8, faceName 32 wide chars }. */
    memset(bank, 0, sizeof(*bank));
    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 4096)
        return false;
    bank->count = count;
    bank->items = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_font_t));

    for (int32_t i = 0; i < count; i++) {
        fxp_font_t *f = &bank->items[i];
        f->handle = fxp_r_u32(r);
        if (build > 284)
            f->handle -= 1;
        f->checksum   = fxp_r_i32(r);
        f->references = fxp_r_i32(r);
        (void)fxp_r_i32(r);              /* size (always 0 in writer)       */
        f->height       = fxp_r_i32(r);
        f->width        = fxp_r_i32(r);
        f->escapement   = fxp_r_i32(r);
        f->orientation  = fxp_r_i32(r);
        f->weight       = fxp_r_i32(r);
        f->italic        = fxp_r_u8(r);
        f->underline    = fxp_r_u8(r);
        f->strike_out   = fxp_r_u8(r);
        f->char_set     = fxp_r_u8(r);
        f->out_precision = fxp_r_u8(r);
        f->clip_precision = fxp_r_u8(r);
        f->quality      = fxp_r_u8(r);
        f->pitch_and_family = fxp_r_u8(r);
        f->face_name = fxp_r_yuniversal(r, 32);
    }
    return fxp_r_ok(r);
}

/* ----------------------------------------------------------------- frames -- */

static void transition_free(fxp_mfa_transition_t *t)
{
    free(t->module);
    free(t->name);
    free(t->params);
    memset(t, 0, sizeof(*t));
}

static bool transition_read(fxp_reader_t *r, fxp_mfa_transition_t *t)
{
    memset(t, 0, sizeof(*t));
    t->module = fxp_r_auto_unicode(r);
    t->name   = fxp_r_auto_unicode(r);
    if (!fxp_r_ascii4(r, t->id) || !fxp_r_ascii4(r, t->transition_id))
        return false;
    t->duration = fxp_r_i32(r);
    t->flags    = fxp_r_i32(r);
    t->color    = fxp_r_color(r);
    int32_t len = fxp_r_i32(r);
    if (len < 0)
        return false;
    t->params_size = len;
    t->params = fxp_malloc((size_t)(len ? len : 1));
    if (len && !fxp_r_read(r, t->params, (size_t)len))
        return false;
    return true;
}

static bool frame_read(fxp_reader_t *r, fxp_mfa_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->handle = fxp_r_i32(r);
    frame->name   = fxp_r_auto_unicode(r);
    frame->size_x = fxp_r_i32(r);
    frame->size_y = fxp_r_i32(r);
    frame->background = fxp_r_color(r);
    frame->flags  = fxp_r_u32(r);

    frame->max_objects = fxp_r_i32(r);
    (void)fxp_r_i32(r);                  /* garbage                         */
    int32_t pw_len = fxp_r_i32(r);
    if (pw_len < 0 || !fxp_r_skip(r, (size_t)pw_len))
        return false;

    frame->last_viewed_x = fxp_r_i32(r);
    frame->last_viewed_y = fxp_r_i32(r);

    (void)fxp_r_i32(r);                  /* palette size field              */
    for (int i = 0; i < 256; i++)
        frame->palette[i] = fxp_r_color(r);

    frame->stamp_handle = fxp_r_i32(r);
    frame->active_layer = fxp_r_i32(r);

    int32_t layers = fxp_r_i32(r);
    if (layers < 0 || layers > 4096)
        return false;
    frame->layer_count = layers;
    frame->layers = fxp_calloc((size_t)(layers ? layers : 1), sizeof(fxp_mfa_layer_t));
    for (int32_t i = 0; i < layers; i++) {
        fxp_mfa_layer_t *l = &frame->layers[i];
        l->name = fxp_r_auto_unicode(r);
        l->flags = fxp_r_u32(r);
        l->x_coefficient = fxp_r_f32(r);
        l->y_coefficient = fxp_r_f32(r);
    }

    frame->has_fade_in = fxp_r_u8(r);
    if (frame->has_fade_in == 1 && !transition_read(r, &frame->fade_in))
        return false;
    frame->has_fade_out = fxp_r_u8(r);
    if (frame->has_fade_out == 1 && !transition_read(r, &frame->fade_out))
        return false;

    int32_t items = fxp_r_i32(r);
    if (items < 0 || items > 65536)
        return false;
    frame->item_count = items;
    frame->items = fxp_calloc((size_t)(items ? items : 1), sizeof(fxp_item_t *));
    for (int32_t i = 0; i < items; i++) {
        if (!fxp_item_read(r, &frame->items[i]))
            return false;
    }

    /* item folders (editor-only; MFAItemFolder header 0x70000004 or single) */
    int32_t folders = fxp_r_i32(r);
    for (int32_t i = 0; i < folders; i++) {
        uint32_t header = fxp_r_u32(r);
        if (header == 0x70000004u) {
            char *name = fxp_r_auto_unicode(r);
            free(name);
            uint32_t n = fxp_r_u32(r);
            if (n > 65536 || !fxp_r_skip(r, (size_t)n * 4))
                return false;
        } else {
            (void)fxp_r_u32(r);          /* single-item folder              */
        }
    }

    int32_t instances = fxp_r_i32(r);
    if (instances < 0 || instances > 1048576)
        return false;
    frame->instance_count = instances;
    frame->instances = fxp_calloc((size_t)(instances ? instances : 1),
                                  sizeof(fxp_mfa_instance_t));
    for (int32_t i = 0; i < instances; i++) {
        fxp_mfa_instance_t *in = &frame->instances[i];
        in->x = fxp_r_i32(r);
        in->y = fxp_r_i32(r);
        in->layer = fxp_r_u32(r);
        in->handle = fxp_r_i32(r);
        in->flags = fxp_r_i16(r);
        in->instance = fxp_r_i16(r);
        in->parent_type = fxp_r_u32(r);
        in->item_handle = fxp_r_u32(r);
        in->parent_handle = fxp_r_i32(r);
    }

    if (!fxp_events_read(r, &frame->events))
        return false;

    fxp_chunk_list_t chunks;
    if (!fxp_chunks_read(r, &chunks))
        return false;
    frame->chunk_count = chunks.count;
    frame->chunks = chunks.items;        /* transfer ownership              */

    return fxp_r_ok(r);
}

/* -------------------------------------------------------------------- app -- */

fxp_item_t *fxp_mfa_frame_item(const fxp_mfa_frame_t *frame, int32_t handle)
{
    for (int32_t i = 0; i < frame->item_count; i++)
        if (frame->items[i]->handle == handle)
            return frame->items[i];
    return NULL;
}

static void binary_files_read(fxp_reader_t *r, fxp_mfa_t *mfa)
{
    /* BinaryFiles.cs: i32 count, { i16 len, ascii name, i32 len, data } */
    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 4096) {
        fxp_log_error("binary files: absurd count %d\n", count);
        count = 0;
    }
    mfa->binary_count = count;
    mfa->binaries = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_binary_file_t));
    for (int32_t i = 0; i < count; i++) {
        fxp_binary_file_t *b = &mfa->binaries[i];
        int16_t nlen = fxp_r_i16(r);
        char name[256];
        if (nlen < 0 || nlen > 255) {
            fxp_log_error("binary file: bad name length %d\n", nlen);
            return;
        }
        if (!fxp_r_read(r, name, (size_t)nlen))
            return;
        name[nlen] = '\0';
        b->name = fxp_strdup(name);
        int32_t dlen = fxp_r_i32(r);
        if (dlen < 0) {
            fxp_log_error("binary file '%s': bad data length\n", b->name);
            return;
        }
        b->size = dlen;
        b->data = fxp_malloc((size_t)(dlen ? dlen : 1));
        if (dlen && !fxp_r_read(r, b->data, (size_t)dlen)) {
            free(b->data);
            b->data = NULL;
            return;
        }
    }
}

/* Free one frame's internals (safe on partially-loaded frames: every
 * pointer is NULL after the memset in frame_read). */
static void frame_free(fxp_mfa_frame_t *f)
{
    if (!f)
        return;
    free(f->name);
    for (int32_t l = 0; l < f->layer_count; l++)
        free(f->layers[l].name);
    free(f->layers);
    transition_free(&f->fade_in);
    transition_free(&f->fade_out);
    for (int32_t it = 0; it < f->item_count; it++)
        fxp_item_free(f->items[it]);
    free(f->items);
    free(f->instances);
    fxp_events_free(&f->events);
    fxp_chunks_free_ptr(f->chunk_count, f->chunks);
    free(f);
}

static void mfa_free_frames(fxp_mfa_t *mfa)
{
    for (int32_t i = 0; i < mfa->frame_count; i++)
        frame_free(mfa->frames[i]);
    free(mfa->frames);
    mfa->frames = NULL;
    mfa->frame_count = 0;
}

void fxp_mfa_free(fxp_mfa_t *mfa)
{
    if (!mfa)
        return;

    free(mfa->name);
    free(mfa->description);
    free(mfa->path);
    free(mfa->author);
    free(mfa->copyright);
    free(mfa->company);
    free(mfa->version_str);
    free(mfa->help_file);
    free(mfa->build_path);
    free(mfa->command_line);
    free(mfa->aboutbox);

    for (int32_t i = 0; i < mfa->fonts.count; i++)
        free(mfa->fonts.items[i].face_name);
    free(mfa->fonts.items);

    for (int32_t i = 0; i < mfa->sounds.count; i++) {
        free(mfa->sounds.items[i].name);
        free(mfa->sounds.items[i].data);
    }
    free(mfa->sounds.items);

    for (int32_t i = 0; i < mfa->music_count; i++) {
        free(mfa->music_items[i].name);
        free(mfa->music_items[i].data);
    }
    free(mfa->music_items);

    for (int32_t i = 0; i < mfa->icons.count; i++)
        fxp_image_free_rgba(&mfa->icons.items[i]);
    free(mfa->icons.items);

    for (int32_t i = 0; i < mfa->images.count; i++)
        fxp_image_free_rgba(&mfa->images.items[i]);
    free(mfa->images.items);

    fxp_value_list_free(&mfa->global_values);
    fxp_value_list_free(&mfa->global_strings);
    free(mfa->global_events);

    free(mfa->icon_images);
    for (int32_t i = 0; i < mfa->qual_count; i++)
        free(mfa->quals[i].name);
    free(mfa->quals);
    for (int32_t i = 0; i < mfa->ext_count; i++) {
        free(mfa->extensions[i].name);
        free(mfa->extensions[i].filename);
        free(mfa->extensions[i].sub_type);
    }
    free(mfa->extensions);

    for (int32_t i = 0; i < mfa->binary_count; i++) {
        free(mfa->binaries[i].name);
        free(mfa->binaries[i].data);
    }
    free(mfa->binaries);
    free(mfa->controls);

    mfa_free_frames(mfa);
    fxp_chunks_free_ptr(mfa->chunk_count, mfa->chunks);
    free(mfa);
}

fxp_mfa_t *fxp_mfa_load(const void *data, size_t size)
{
    fxp_reader_t r;
    fxp_r_init(&r, data, size);

    char magic[5];
    if (!fxp_r_ascii4(&r, magic)) {
        fxp_log_error("mfa: file too small\n");
        return NULL;
    }
    if (strcmp(magic, FXP_MFA_MAGIC) != 0) {
        fxp_log_error("mfa: bad magic '%s' (expected %s)\n", magic, FXP_MFA_MAGIC);
        return NULL;
    }

    fxp_mfa_t *mfa = fxp_calloc(1, sizeof(fxp_mfa_t));
    mfa->version    = fxp_r_i16(&r);
    mfa->subversion = fxp_r_i16(&r);
    mfa->product    = fxp_r_i32(&r);
    mfa->build      = fxp_r_i32(&r);
    mfa->lang_id    = fxp_r_i32(&r);
    fxp_log_info("mfa: version %d.%d product %d build %d",
                 mfa->version, mfa->subversion, mfa->product, mfa->build);

    mfa->name        = fxp_r_auto_unicode(&r);
    mfa->description = fxp_r_auto_unicode(&r);
    mfa->path        = fxp_r_auto_unicode(&r);
    int32_t stamp_len = fxp_r_i32(&r);
    if (stamp_len < 0 || !fxp_r_skip(&r, (size_t)stamp_len))
        goto fail;

    /* font bank "ATNF" */
    char tag[5];
    if (!fxp_r_ascii4(&r, tag) || strcmp(tag, FXP_BANK_FONTS) != 0) {
        fxp_log_error("mfa: expected font bank, got '%s'\n", tag);
        goto fail;
    }
    if (!font_bank_read(&r, &mfa->fonts, mfa->build))
        goto fail;

    /* sound bank "APMS" */
    if (!fxp_r_ascii4(&r, tag) || strcmp(tag, FXP_BANK_SOUNDS) != 0) {
        fxp_log_error("mfa: expected sound bank, got '%s'\n", tag);
        goto fail;
    }
    if (!sound_bank_read(&r, &mfa->sounds))
        goto fail;

    /* music bank "ASUM" — items are zlib-wrapped
     * { checksum, refs, size, flags, reserved, nameLen, name, data }
     * (MusicBank.cs MusicFile.Read).  Kept for the audio decoder. */
    if (!fxp_r_ascii4(&r, tag) || strcmp(tag, FXP_BANK_MUSIC) != 0) {
        fxp_log_error("mfa: expected music bank, got '%s'\n", tag);
        goto fail;
    }
    mfa->music_count = fxp_r_i32(&r);
    if (mfa->music_count < 0 || mfa->music_count > 65536)
        goto fail;
    if (mfa->music_count > 0) {
        mfa->music_items = fxp_calloc((size_t)mfa->music_count,
                                      sizeof(fxp_music_t));
        for (int32_t i = 0; i < mfa->music_count; i++) {
            fxp_music_t *mu = &mfa->music_items[i];
            mu->handle = fxp_r_i32(&r);
            int32_t decomp = fxp_r_i32(&r);
            int32_t comp = fxp_r_i32(&r);
            if (decomp < 0 || comp < 0 ||
                fxp_r_tell(&r) + (size_t)comp > fxp_r_size(&r)) {
                fxp_log_error("music %d: bad sizes\n", mu->handle);
                goto fail;
            }
            uint8_t *compdata = fxp_malloc((size_t)(comp ? comp : 1));
            bool okrd = comp ? fxp_r_read(&r, compdata, (size_t)comp) : true;
            uint8_t *inflated = NULL;
            size_t got = 0;
            if (okrd && comp)
                inflated = fxp_zlib_inflate(compdata, (size_t)comp,
                                            (size_t)decomp, &got);
            free(compdata);
            if (okrd && comp && !inflated) {
                fxp_log_warn("music %d: inflate failed — item skipped\n",
                             mu->handle);
                continue;
            }
            if (!inflated)
                continue;
            uint8_t *blob = inflated;
            if ((int32_t)got < 20) {
                free(blob);
                continue;
            }
            fxp_reader_t sr;
            fxp_r_init(&sr, blob, got);
            (void)fxp_r_i32(&sr);         /* checksum   */
            (void)fxp_r_i32(&sr);         /* references */
            uint32_t size = fxp_r_u32(&sr);
            (void)fxp_r_u32(&sr);         /* flags      */
            (void)fxp_r_i32(&sr);         /* reserved   */
            int32_t name_len = fxp_r_i32(&sr);
            if (name_len < 0 || name_len > 4096) {
                free(blob);
                continue;
            }
            mu->name = fxp_r_utf16(&sr, name_len);
            int32_t data_len = (int32_t)size - name_len * 2;
            if (data_len < 0 || (size_t)(name_len * 2 + data_len) > got)
                data_len = (int32_t)(got - (size_t)name_len * 2);
            if (data_len > 0) {
                mu->data = fxp_malloc((size_t)data_len);
                memcpy(mu->data, blob + (size_t)name_len * 2, (size_t)data_len);
            }
            mu->data_size = data_len > 0 ? data_len : 0;
            free(blob);
        }
        fxp_log_info("mfa: music bank has %d item(s)", mfa->music_count);
    }

    /* icon bank "AGMI" */
    if (!fxp_r_ascii4(&r, tag) || strcmp(tag, FXP_BANK_IMAGES) != 0) {
        fxp_log_error("mfa: expected icon bank, got '%s'\n", tag);
        goto fail;
    }
    if (!image_bank_read(&r, &mfa->icons))
        goto fail;

    /* image bank "AGMI" */
    if (!fxp_r_ascii4(&r, tag) || strcmp(tag, FXP_BANK_IMAGES) != 0) {
        fxp_log_error("mfa: expected image bank, got '%s'\n", tag);
        goto fail;
    }
    if (!image_bank_read(&r, &mfa->images))
        goto fail;

    /* second identity block */
    free(fxp_r_auto_unicode(&r));        /* name (repeat)                   */
    mfa->author    = fxp_r_auto_unicode(&r);
    free(fxp_r_auto_unicode(&r));        /* description (repeat)            */
    mfa->copyright = fxp_r_auto_unicode(&r);
    mfa->company   = fxp_r_auto_unicode(&r);
    mfa->version_str = fxp_r_auto_unicode(&r);

    mfa->window_x = fxp_r_i32(&r);
    mfa->window_y = fxp_r_i32(&r);
    mfa->border_color = fxp_r_color(&r);
    mfa->display_flags = fxp_r_u32(&r);
    mfa->graphic_flags = fxp_r_u32(&r);
    mfa->help_file = fxp_r_auto_unicode(&r);
    free(fxp_r_auto_unicode(&r));        /* unknown string                  */

    mfa->initial_score = fxp_r_i32(&r);
    mfa->initial_lives = fxp_r_i32(&r);
    mfa->frame_rate    = fxp_r_i32(&r);
    mfa->build_type    = fxp_r_i32(&r);
    mfa->build_path    = fxp_r_auto_unicode(&r);
    free(fxp_r_auto_unicode(&r));        /* unknown string 2                */
    mfa->command_line  = fxp_r_auto_unicode(&r);
    mfa->aboutbox      = fxp_r_auto_unicode(&r);
    (void)fxp_r_u32(&r);                 /* always zero                     */

    binary_files_read(&r, mfa);

    /* MFAControls (MFAControls.cs): i32 count; { i32 type, i32 16, 16×i32 } */
    {
        int32_t count = fxp_r_i32(&r);
        if (count < 0 || count > 8)
            goto fail;
        mfa->control_count = count;
        mfa->controls = fxp_calloc((size_t)(count ? count : 1),
                                   sizeof(fxp_player_control_t));
        for (int32_t i = 0; i < count; i++) {
            mfa->controls[i].control_type = fxp_r_i32(&r);
            int32_t nkeys = fxp_r_i32(&r);       /* 16                      */
            if (nkeys != 16) {
                fxp_log_error("controls: expected 16 keys, got %d\n", nkeys);
                goto fail;
            }
            for (int k = 0; k < 16; k++)
                mfa->controls[i].keys[k] = fxp_r_i32(&r);
        }
    }

    /* menu block: skipped wholesale via its size prefix */
    {
        uint32_t menu_size = fxp_r_u32(&r);
        if (!fxp_r_skip(&r, menu_size))
            goto fail;
    }
    (void)fxp_r_i32(&r);                 /* window menu index               */
    {
        int32_t mi = fxp_r_i32(&r);
        if (mi < 0 || mi > 4096 || !fxp_r_skip(&r, (size_t)mi * 8))
            goto fail;                    /* { i32 id, i32 image } pairs     */
    }

    if (!fxp_value_list_read(&r, &mfa->global_values) ||
        !fxp_value_list_read(&r, &mfa->global_strings))
        goto fail;

    mfa->global_events_size = fxp_r_i32(&r);
    if (mfa->global_events_size < 0)
        goto fail;
    mfa->global_events = fxp_malloc((size_t)(mfa->global_events_size
                                              ? mfa->global_events_size : 1));
    if (mfa->global_events_size &&
        !fxp_r_read(&r, mfa->global_events, (size_t)mfa->global_events_size))
        goto fail;

    mfa->graphic_mode = fxp_r_i32(&r);

    mfa->icon_count = fxp_r_i32(&r);
    if (mfa->icon_count < 0 || mfa->icon_count > 256)
        goto fail;
    mfa->icon_images = fxp_malloc((size_t)(mfa->icon_count ? mfa->icon_count : 1)
                                  * sizeof(int32_t));
    for (int32_t i = 0; i < mfa->icon_count; i++)
        mfa->icon_images[i] = fxp_r_i32(&r);

    mfa->qual_count = fxp_r_i32(&r);
    if (mfa->qual_count < 0 || mfa->qual_count > 4096)
        goto fail;
    mfa->quals = fxp_calloc((size_t)(mfa->qual_count ? mfa->qual_count : 1),
                            sizeof(fxp_qual_entry_t));
    for (int32_t i = 0; i < mfa->qual_count; i++) {
        int32_t nlen = fxp_r_i32(&r);
        if (nlen < 0 || nlen > 1024)
            goto fail;
        char nm[1025];
        if (nlen && !fxp_r_read(&r, nm, (size_t)nlen))
            goto fail;
        nm[nlen] = '\0';
        mfa->quals[i].name = fxp_strdup(nm);
        mfa->quals[i].handle = fxp_r_i32(&r);
    }

    mfa->ext_count = fxp_r_i32(&r);
    if (mfa->ext_count < 0 || mfa->ext_count > 4096)
        goto fail;
    mfa->extensions = fxp_calloc((size_t)(mfa->ext_count ? mfa->ext_count : 1),
                                 sizeof(fxp_ext_entry_t));
    for (int32_t i = 0; i < mfa->ext_count; i++) {
        fxp_ext_entry_t *e = &mfa->extensions[i];
        e->handle   = fxp_r_i32(&r);
        e->name     = fxp_r_auto_unicode(&r);
        e->filename = fxp_r_auto_unicode(&r);
        e->magic    = fxp_r_i32(&r);
        int32_t sub_len = fxp_r_i32(&r);
        e->sub_type = fxp_r_utf16(&r, sub_len);
    }

    /* the "> 900" quirk (MFAData.cs:430-433) */
    if (fxp_r_peek_i32(&r) > 900)
        (void)fxp_r_i16(&r);

    /* frame offset table */
    int32_t off_count = fxp_r_i32(&r);
    if (off_count < 0 || off_count > 65536)
        goto fail;
    int32_t *offsets = fxp_malloc((size_t)(off_count ? off_count : 1) * sizeof(int32_t));
    for (int32_t i = 0; i < off_count; i++)
        offsets[i] = fxp_r_i32(&r);

    int32_t next_offset = fxp_r_i32(&r);

    mfa->frame_count = off_count;
    mfa->frames = fxp_calloc((size_t)(off_count ? off_count : 1),
                             sizeof(fxp_mfa_frame_t *));
    for (int32_t i = 0; i < off_count; i++) {
        fxp_mfa_frame_t *frame = fxp_malloc(sizeof(fxp_mfa_frame_t));
        if (!fxp_r_seek(&r, (size_t)offsets[i]) || !frame_read(&r, frame)) {
            fxp_log_error("mfa: frame %d failed to load\n", i);
            frame_free(frame);            /* frees partial allocations    */
            free(offsets);
            goto fail;
        }
        mfa->frames[i] = frame;
        fxp_log_info("mfa: frame %d '%s' (%dx%d) items=%d instances=%d groups=%d",
                     i, frame->name, frame->size_x, frame->size_y,
                     frame->item_count, frame->instance_count,
                     frame->events.group_count);
    }
    free(offsets);

    /* app-level mini-chunks */
    if (!fxp_r_seek(&r, (size_t)next_offset)) {
        /* non-fatal: some MFAs end here */
    }
    {
        fxp_chunk_list_t chunks;
        if (fxp_r_ok(&r) && fxp_chunks_read(&r, &chunks)) {
            mfa->chunk_count = chunks.count;
            mfa->chunks = chunks.items;
        }
    }

    fxp_log_info("mfa: loaded '%s' — %d frame(s), %d image(s), %d sound(s)",
                 mfa->name, mfa->frame_count, mfa->images.count, mfa->sounds.count);
    return mfa;

fail:
    fxp_log_error("mfa: load failed at offset %zu", fxp_r_tell(&r));
    fxp_mfa_free(mfa);
    return NULL;
}
