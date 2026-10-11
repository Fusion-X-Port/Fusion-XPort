/*
 * Fusion X-Port — mfa_objects.c
 * Frame item (object info) loaders: common base, Active animations,
 * Backdrop / Quick Backdrop, Text, Counter, Extension.  Field order matches
 * CTFAK's MFA/MFAObjectLoaders sources exactly.
 */
#include "mfa_internal.h"
#include "fxp/mfa_format.h"

/* ----------------------------------------------------------- value lists -- */

bool fxp_value_list_read(fxp_reader_t *r, fxp_value_list_t *out)
{
    memset(out, 0, sizeof(*out));
    int32_t count = fxp_r_i32(r);
    if (count < 0)
        return false;
    out->count = count;
    out->items = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_value_item_t));

    for (int32_t i = 0; i < count; i++) {
        fxp_value_item_t *it = &out->items[i];
        it->name = fxp_r_auto_unicode(r);
        it->type = fxp_r_i32(r);
        switch (it->type) {
        case FXP_VTYPE_STRING:
            it->s = fxp_r_auto_unicode(r);
            break;
        case FXP_VTYPE_INT:
            it->i = fxp_r_i32(r);
            break;
        case FXP_VTYPE_FLOAT:
            it->f = fxp_r_f32(r);
            break;
        default:
            fxp_log_warn("value item '%s': unknown type %d\n", it->name, it->type);
            it->i = fxp_r_i32(r);
            break;
        }
    }
    return fxp_r_ok(r);
}

void fxp_value_list_free(fxp_value_list_t *list)
{
    for (int32_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].s);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* ------------------------------------------------------------- movements -- */

/*
 * Movement loader params, per CTFAK's Movements.cs loaders (all little
 * endian, immediately after the 12-byte header):
 *   Ball (4):        speed, randomizer, angles, security, deceleration
 *   EightDir (3):    speed, acceleration, deceleration, bounceFactor,
 *                    directions (i32 mask)
 *   Race (2):        speed, acceleration, deceleration, rotationSpeed,
 *                    bounceFactor, angles, reverse
 *   Platform (9):    speed, acceleration, deceleration, control, gravity,
 *                    jumpStrength
 */
static int32_t mov_i16(const uint8_t *d, int32_t size, int idx)
{
    int32_t off = idx * 2;
    if (!d || off + 2 > size)
        return 0;
    return (int32_t)(int16_t)(d[off] | (d[off + 1] << 8));
}

static int32_t mov_i32(const uint8_t *d, int32_t size, int idx)
{
    int32_t off = idx * 4;
    if (!d || off + 4 > size)
        return 0;
    return (int32_t)((uint32_t)d[off] |
                     ((uint32_t)d[off + 1] << 8) |
                     ((uint32_t)d[off + 2] << 16) |
                     ((uint32_t)d[off + 3] << 24));
}

void fxp_movement_parse_params(fxp_movement_t *m)
{
    const uint8_t *d = m->data;
    int32_t n = m->data_size;
    switch (m->type) {
    case FXP_MOV_BOUNCING_BALL:
        m->mv_speed        = mov_i16(d, n, 0);
        m->mv_randomizer   = mov_i16(d, n, 1);
        m->mv_angles       = mov_i16(d, n, 2);
        m->mv_security     = mov_i16(d, n, 3);
        m->mv_deceleration = mov_i16(d, n, 4);
        break;
    case FXP_MOV_EIGHT_DIRECTIONS:
        m->mv_speed         = mov_i16(d, n, 0);
        m->mv_acceleration  = mov_i16(d, n, 1);
        m->mv_deceleration  = mov_i16(d, n, 2);
        m->mv_bounce_factor = mov_i16(d, n, 3);
        m->mv_directions    = mov_i32(d, n, 2);       /* i32 at byte 8    */
        break;
    case FXP_MOV_RACE:
        m->mv_speed         = mov_i16(d, n, 0);
        m->mv_acceleration  = mov_i16(d, n, 1);
        m->mv_deceleration  = mov_i16(d, n, 2);
        m->mv_bounce_factor = mov_i16(d, n, 4);
        m->mv_angles        = mov_i16(d, n, 5);
        break;
    case FXP_MOV_PLATFORM:
        m->mv_speed        = mov_i16(d, n, 0);
        m->mv_acceleration = mov_i16(d, n, 1);
        m->mv_deceleration = mov_i16(d, n, 2);
        break;
    default:
        break;
    }
}

static void movements_free(int32_t count, fxp_movement_t *movs)
{
    for (int32_t i = 0; i < count; i++) {
        free(movs[i].name);
        free(movs[i].extension);
        free(movs[i].data);
    }
    free(movs);
}

static bool movements_read(fxp_reader_t *r, int32_t *out_count, fxp_movement_t **out)
{
    uint32_t count = fxp_r_u32(r);
    if (count > 4096) {
        fxp_log_error("movements: absurd count %u\n", count);
        return false;
    }
    fxp_movement_t *movs = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_movement_t));
    for (uint32_t i = 0; i < count; i++) {
        fxp_movement_t *m = &movs[i];
        m->name      = fxp_r_auto_unicode(r);
        m->extension = fxp_r_auto_unicode(r);
        m->identifier = fxp_r_u32(r);
        int32_t data_size = (int32_t)fxp_r_u32(r);
        if (m->extension[0] != '\0') {
            /* extension movement: whole blob is opaque */
            m->data_size = data_size;
            m->data = fxp_malloc((size_t)(data_size > 0 ? data_size : 1));
            if (!fxp_r_read(r, m->data, (size_t)(data_size > 0 ? data_size : 0))) {
                movements_free((int32_t)i + 1, movs);
                return false;
            }
        } else {
            m->player          = fxp_r_u16(r);
            m->type            = fxp_r_u16(r);
            m->moving_at_start = fxp_r_u8(r);
            fxp_r_skip(r, 3);
            m->direction_at_start = fxp_r_i32(r);
            int32_t rest = data_size - 12;
            if (rest < 0) rest = 0;
            m->data_size = rest;
            m->data = fxp_malloc((size_t)(rest ? rest : 1));
            if (rest && !fxp_r_read(r, m->data, (size_t)rest)) {
                movements_free((int32_t)i + 1, movs);
                return false;
            }
            fxp_movement_parse_params(m);
        }
    }
    *out_count = (int32_t)count;
    *out = movs;
    return fxp_r_ok(r);
}

/* ----------------------------------------------------------- common base -- */

static void common_free(fxp_item_common_t *c)
{
    fxp_value_list_free(&c->values);
    fxp_value_list_free(&c->strings);
    movements_free(c->movement_count, c->movements);
    for (int32_t i = 0; i < c->behaviour_count; i++) {
        free(c->behaviour_names[i]);
        free(c->behaviour_data[i]);
    }
    free(c->behaviour_names);
    free(c->behaviour_data);
    free(c->behaviour_sizes);
    memset(c, 0, sizeof(*c));
}

static bool common_read(fxp_reader_t *r, fxp_item_common_t *c)
{
    memset(c, 0, sizeof(*c));
    c->object_flags     = fxp_r_i32(r);
    c->new_object_flags = fxp_r_i32(r);
    c->back_color       = fxp_r_color(r);

    for (int i = 0; i < 8; i++)          /* 8 qualifiers + terminator (i16) */
        c->qualifiers[i] = fxp_r_i16(r);
    (void)fxp_r_i16(r);

    if (!fxp_value_list_read(r, &c->values) ||
        !fxp_value_list_read(r, &c->strings))
        return false;

    if (!movements_read(r, &c->movement_count, &c->movements))
        return false;

    /* Behaviours (MFABehaviours.cs): i32 count, { AutoUnicode name,
     * u32 len, data } */
    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 4096)
        return false;
    c->behaviour_count = count;
    if (count > 0) {
        c->behaviour_names = fxp_calloc((size_t)count, sizeof(char *));
        c->behaviour_data  = fxp_calloc((size_t)count, sizeof(uint8_t *));
        c->behaviour_sizes = fxp_calloc((size_t)count, sizeof(int32_t));
    }
    for (int32_t i = 0; i < count; i++) {
        c->behaviour_names[i] = fxp_r_auto_unicode(r);
        uint32_t len = fxp_r_u32(r);
        c->behaviour_sizes[i] = (int32_t)len;
        c->behaviour_data[i]  = fxp_malloc(len ? len : 1);
        if (len && !fxp_r_read(r, c->behaviour_data[i], len))
            return false;
    }

    fxp_r_skip(r, 2);                    /* transitions (ObjectLoader) */
    return fxp_r_ok(r);
}

/* -------------------------------------------------------------- animations */

static void anims_free(int32_t count, fxp_anim_t *anims)
{
    for (int32_t i = 0; i < count; i++) {
        free(anims[i].name);
        for (int32_t j = 0; j < anims[i].dir_count; j++)
            free(anims[i].dirs[j].frames);
        free(anims[i].dirs);
    }
    free(anims);
}

static bool anims_read(fxp_reader_t *r, int32_t *out_count, fxp_anim_t **out)
{
    uint32_t count = fxp_r_u32(r);
    if (count > 256) {
        fxp_log_error("animations: absurd count %u\n", count);
        return false;
    }
    fxp_anim_t *anims = fxp_calloc((size_t)(count ? count : 1), sizeof(fxp_anim_t));
    for (uint32_t i = 0; i < count; i++) {
        fxp_anim_t *a = &anims[i];
        a->name = fxp_r_auto_unicode(r);
        int32_t dirs = fxp_r_i32(r);
        if (dirs < 0 || dirs > 64) {
            anims_free((int32_t)i + 1, anims);
            return false;
        }
        a->dir_count = dirs;
        a->dirs = fxp_calloc((size_t)(dirs ? dirs : 1), sizeof(fxp_anim_dir_t));
        for (int32_t d = 0; d < dirs; d++) {
            fxp_anim_dir_t *dir = &a->dirs[d];
            dir->index    = fxp_r_i32(r);
            dir->min_speed = fxp_r_i32(r);
            dir->max_speed = fxp_r_i32(r);
            dir->repeat   = fxp_r_i32(r);
            dir->back_to  = fxp_r_i32(r);
            int32_t nf = fxp_r_i32(r);
            if (nf < 0 || nf > 65536) {
                anims_free((int32_t)i + 1, anims);
                return false;
            }
            dir->frame_count = nf;
            dir->frames = fxp_malloc((size_t)(nf ? nf : 1) * sizeof(int32_t));
            for (int32_t f = 0; f < nf; f++)
                dir->frames[f] = fxp_r_i32(r);
        }
    }
    *out_count = (int32_t)count;
    *out = anims;
    return fxp_r_ok(r);
}

/* ---------------------------------------------------------------- loaders */

static bool backdrop_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    /* MFABackgroundLoader: u32 obstacle, u32 collision, i32 image handle */
    item->as.backdrop.obstacle_type = (int32_t)fxp_r_u32(r);
    item->as.backdrop.collision_type = (int32_t)fxp_r_u32(r);
    item->as.backdrop.image = fxp_r_i32(r);
    return fxp_r_ok(r);
}

static bool quick_backdrop_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    item->as.quick_backdrop.obstacle     = (int32_t)fxp_r_u32(r);
    item->as.quick_backdrop.collision    = (int32_t)fxp_r_u32(r);
    item->as.quick_backdrop.w            = fxp_r_i32(r);
    item->as.quick_backdrop.h            = fxp_r_i32(r);
    item->as.quick_backdrop.shape        = fxp_r_i32(r);
    item->as.quick_backdrop.border_size  = fxp_r_i32(r);
    item->as.quick_backdrop.border_color = fxp_r_color(r);
    item->as.quick_backdrop.fill_type    = fxp_r_i32(r);
    item->as.quick_backdrop.color1       = fxp_r_color(r);
    item->as.quick_backdrop.color2       = fxp_r_color(r);
    item->as.quick_backdrop.flags        = fxp_r_i32(r);
    item->as.quick_backdrop.image        = fxp_r_i32(r);
    return fxp_r_ok(r);
}

static bool active_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    uint8_t has = fxp_r_u8(r);
    item->as.active.has_anims = has;
    if (has != 0) {
        if (!anims_read(r, &item->as.active.anim_count, &item->as.active.anims))
            return false;
    }
    return fxp_r_ok(r);
}

static bool text_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    item->as.text.w    = fxp_r_i32(r);
    item->as.text.h    = fxp_r_i32(r);
    item->as.text.font = fxp_r_i32(r);
    item->as.text.color = fxp_r_color(r);
    item->as.text.flags = fxp_r_u32(r);
    (void)fxp_r_u32(r);                  /* always zero (MFAText.cs) */
    int32_t count = fxp_r_i32(r);
    if (count < 0 || count > 65536)
        return false;
    item->as.text.paragraph_count = count;
    item->as.text.paragraphs = fxp_calloc((size_t)(count ? count : 1),
                                          sizeof(fxp_paragraph_t));
    for (int32_t i = 0; i < count; i++) {
        item->as.text.paragraphs[i].text  = fxp_r_auto_unicode(r);
        item->as.text.paragraphs[i].flags = fxp_r_u32(r);
    }
    return fxp_r_ok(r);
}

static bool counter_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    item->as.counter.value = fxp_r_i32(r);
    item->as.counter.min   = fxp_r_i32(r);
    item->as.counter.max   = fxp_r_i32(r);
    item->as.counter.display_type = fxp_r_u32(r);
    item->as.counter.count_flags  = fxp_r_u32(r);
    item->as.counter.color1 = fxp_r_color(r);
    item->as.counter.color2 = fxp_r_color(r);
    item->as.counter.vertical_gradient = fxp_r_i32(r);
    item->as.counter.count_type = fxp_r_i32(r);
    item->as.counter.w = fxp_r_i32(r);
    item->as.counter.h = fxp_r_i32(r);
    uint32_t imgs = fxp_r_u32(r);
    if (imgs > 4096)
        return false;
    item->as.counter.image_count = (int32_t)imgs;
    item->as.counter.images = fxp_malloc((size_t)(imgs ? imgs : 1) * sizeof(int32_t));
    for (uint32_t i = 0; i < imgs; i++)
        item->as.counter.images[i] = fxp_r_i32(r);
    item->as.counter.font = fxp_r_i32(r);
    return fxp_r_ok(r);
}

/* Score (5) / Lives (6) — MFALives.cs (used by CTFAK's writer, so it is
 * the layout Fusion's editor reads and writes too): u32 player,
 * i32 image count + handles, i32 displayType, i32 flags, i32 font,
 * i32 width, i32 height. */
static bool lives_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    item->as.lives.player = (int32_t)fxp_r_u32(r);
    uint32_t imgs = fxp_r_u32(r);
    if (imgs > 4096)
        return false;
    item->as.lives.image_count = (int32_t)imgs;
    item->as.lives.images = fxp_malloc((size_t)(imgs ? imgs : 1) *
                                       sizeof(int32_t));
    for (uint32_t i = 0; i < imgs; i++)
        item->as.lives.images[i] = fxp_r_i32(r);
    item->as.lives.display_type = fxp_r_i32(r);
    item->as.lives.flags        = fxp_r_i32(r);
    item->as.lives.font         = fxp_r_i32(r);
    item->as.lives.w            = fxp_r_i32(r);
    item->as.lives.h            = fxp_r_i32(r);
    return fxp_r_ok(r);
}

/* SubApplication (9) — MFASubApplication.cs writer layout: unicode
 * fileName, i32 width, i32 height, i32 flag, i32 frameNum. */
static bool subapp_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    item->as.subapp.file_name = fxp_r_auto_unicode(r);
    item->as.subapp.w         = fxp_r_i32(r);
    item->as.subapp.h         = fxp_r_i32(r);
    item->as.subapp.flag      = fxp_r_i32(r);
    item->as.subapp.frame_num = fxp_r_i32(r);
    return fxp_r_ok(r);
}

/* ------------------------------------------- unknown bodies (scan skip) --
 *
 * MFA frame items have NO length prefix, so an unknown body cannot be
 * "skipped" directly.  Instead, scan forward for the next parse point:
 * either the start of the next frame item or the item-folder header that
 * closes the item list (u32 0x70000004 / 0x70000005 — MFAItemFolder).
 * This keeps the whole MFA loadable even when it contains object types
 * the runtime does not model (Question, RTF, ...). */

#define FXP_FOLDER_HEADER_1 0x70000004u
#define FXP_FOLDER_HEADER_2 0x70000005u

static uint32_t scan_rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#define rd_u32 scan_rd_u32

/* Sanity-check an AutoUnicode string at `pos` without consuming. */
static bool peek_unicode_ok(const uint8_t *base, size_t size, size_t pos)
{
    if (pos + 4 > size)
        return false;
    int32_t len = (int16_t)(base[pos] | (base[pos + 1] << 8));
    if (len < 0 || len > 4096)
        return false;
    if (base[pos + 2] != 0x00 || base[pos + 3] != 0x80)
        return false;
    return pos + 4u + (size_t)len * 2u <= size;
}

/* Does a frame item begin at `pos`?  Signature: i32 type (known or a
 * plausible extension), i32 handle, unicode name, 6× i32 fields ending in
 * icon_type == 1, then icon handle. */
static bool looks_like_item_start(const uint8_t *base, size_t size, size_t pos)
{
    if (pos + 12 > size)
        return false;
    int32_t type = (int32_t)rd_u32(base + pos);
    int32_t handle = (int32_t)rd_u32(base + pos + 4);
    bool known = (type >= 0 && type <= 9) || (type >= 32 && type < 2048);
    if (!known || handle < 0 || handle > 100000)
        return false;
    if (!peek_unicode_ok(base, size, pos + 8))
        return false;
    size_t p = pos + 8 + 4 + (size_t)((int16_t)(base[pos + 8] |
                                                (base[pos + 9] << 8))) * 2u;
    if (p + 28 > size)
        return false;                     /* 6× i32 + i32 icon handle      */
    int32_t icon_type = (int32_t)rd_u32(base + p + 20);
    int32_t icon_handle = (int32_t)rd_u32(base + p + 24);
    if (icon_type != 1)
        return false;
    if (icon_handle < -1 || icon_handle > 100000)
        return false;
    return true;
}

/* Does an item-folder header begin at `pos` and parse sanely? */
static bool looks_like_folder_start(const uint8_t *base, size_t size,
                                    size_t pos)
{
    if (pos + 4 > size)
        return false;
    uint32_t hdr = rd_u32(base + pos);
    if (hdr != FXP_FOLDER_HEADER_1 && hdr != FXP_FOLDER_HEADER_2)
        return false;
    if (hdr == FXP_FOLDER_HEADER_2)
        return pos + 8 <= size;           /* u32 header + u32 handle       */
    /* 0x70000004: unicode name + u32 count + count × u32 handles */
    if (!peek_unicode_ok(base, size, pos + 4))
        return false;
    size_t p = pos + 4 + 4 +
               (size_t)((int16_t)(base[pos + 4] | (base[pos + 5] << 8))) * 2u;
    if (p + 4 > size)
        return false;
    uint32_t count = rd_u32(base + p);
    return count <= 65536 && p + 4 + (size_t)count * 4u <= size;
}

static bool unknown_body_skip(fxp_reader_t *r, fxp_item_t *item)
{
    const uint8_t *base = fxp_r_ptr(r);   /* points at the current pos    */
    size_t size = fxp_r_size(r) - fxp_r_tell(r);   /* remaining bytes     */
    size_t start = 0;
    fxp_log_warn("item '%s' (type %d): unknown body — scanning for the "
                 "next parse point", item->name, item->object_type);
    for (size_t pos = start; pos + 4 <= size; pos++) {
        if (looks_like_item_start(base, size, pos) ||
            looks_like_folder_start(base, size, pos)) {
            fxp_log_warn("item '%s': skipped %zu byte(s) of unknown body",
                         item->name, pos - start);
            return fxp_r_skip(r, pos);
        }
    }
    return false;
}

static bool extension_body_read(fxp_reader_t *r, fxp_item_t *item)
{
    /* MFAExtensionObject: i32 -1, name, filename, magic u32, wide subType,
     * then { i32 size(-20); 4 bytes; i32 version; i32 id; i32 private; data } */
    item->as.extension.ext_type = fxp_r_i32(r);
    item->as.extension.ext_name = fxp_r_auto_unicode(r);
    item->as.extension.ext_filename = fxp_r_auto_unicode(r);
    item->as.extension.magic = fxp_r_i32(r);
    int32_t sub_len = fxp_r_i32(r);
    item->as.extension.ext_sub_type = fxp_r_utf16(r, sub_len);

    int32_t block = fxp_r_i32(r);        /* size-20                          */
    (void)fxp_r_i32(r);                  /* 4 bytes                          */
    item->as.extension.ext_version  = fxp_r_u32(r);
    item->as.extension.ext_id       = fxp_r_u32(r);
    item->as.extension.ext_private  = fxp_r_u32(r);
    int32_t data = block - 12;
    if (data < 0)
        data = 0;
    item->as.extension.data_size = data;
    item->as.extension.data = fxp_malloc((size_t)(data ? data : 1));
    if (data && !fxp_r_read(r, item->as.extension.data, (size_t)data))
        return false;
    return fxp_r_ok(r);
}

/* ------------------------------------------------------------- item itself */

void fxp_item_free(fxp_item_t *item)
{
    if (!item)
        return;
    free(item->name);
    common_free(&item->common);

    switch (item->object_type) {
    case FXP_OT_ACTIVE:
        anims_free(item->as.active.anim_count, item->as.active.anims);
        break;
    case FXP_OT_TEXT:
        for (int32_t i = 0; i < item->as.text.paragraph_count; i++)
            free(item->as.text.paragraphs[i].text);
        free(item->as.text.paragraphs);
        break;
    case FXP_OT_COUNTER:
        free(item->as.counter.images);
        break;
    case FXP_OT_SCORE:
    case FXP_OT_LIVES:
        free(item->as.lives.images);
        break;
    case FXP_OT_SUBAPPLICATION:
        free(item->as.subapp.file_name);
        break;
    default:
        if (item->object_type >= FXP_OT_EXTENSION) {
            free(item->as.extension.ext_name);
            free(item->as.extension.ext_filename);
            free(item->as.extension.ext_sub_type);
            free(item->as.extension.data);
        }
        break;
    }
    free(item);
}

bool fxp_item_read(fxp_reader_t *r, fxp_item_t **out)
{
    fxp_item_t *item = fxp_calloc(1, sizeof(fxp_item_t));
    item->object_type = fxp_r_i32(r);
    item->handle      = fxp_r_i32(r);
    item->name        = fxp_r_auto_unicode(r);
    item->transparent = fxp_r_i32(r);
    item->ink_effect  = fxp_r_i32(r);
    item->ink_effect_param = fxp_r_u32(r);
    item->anti_aliasing = fxp_r_i32(r);
    item->flags       = fxp_r_i32(r);
    item->icon_type   = fxp_r_i32(r);
    if (item->icon_type == 1)
        item->icon_handle = fxp_r_i32(r);
    else {
        fxp_log_error("item '%s': icon type %d != 1\n", item->name, item->icon_type);
        fxp_item_free(item);
        return false;
    }

    /* editor mini-chunks — parsed and stored raw */
    fxp_chunk_list_t chunks;
    if (!fxp_chunks_read(r, &chunks)) {
        fxp_item_free(item);
        return false;
    }
    fxp_chunks_free(&chunks);

    bool ok;
    if (item->object_type >= FXP_OT_EXTENSION)
        ok = common_read(r, &item->common) && extension_body_read(r, item);
    else {
        ok = common_read(r, &item->common);
        if (ok) {
            switch (item->object_type) {
            case FXP_OT_QUICK_BACKDROP: ok = quick_backdrop_body_read(r, item); break;
            case FXP_OT_BACKDROP:       ok = backdrop_body_read(r, item);       break;
            case FXP_OT_ACTIVE:         ok = active_body_read(r, item);         break;
            case FXP_OT_TEXT:           ok = text_body_read(r, item);           break;
            case FXP_OT_COUNTER:        ok = counter_body_read(r, item);        break;
            case FXP_OT_SCORE:          ok = lives_body_read(r, item);          break;
            case FXP_OT_LIVES:          ok = lives_body_read(r, item);          break;
            case FXP_OT_SUBAPPLICATION: ok = subapp_body_read(r, item);         break;
            default:
                /* Question, RTF, ...: keep the MFA loadable by scanning
                 * past the unknown body. */
                ok = unknown_body_skip(r, item);
                break;
            }
        }
    }

    if (!ok) {
        fxp_log_error("item '%s' (type %d) failed to load\n",
                      item->name, item->object_type);
        fxp_item_free(item);
        return false;
    }
    *out = item;
    return true;
}
