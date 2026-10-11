/*
 * Fusion X-Port — mfa.h
 * In-memory model of a Clickteam Fusion MFA project plus its loader.
 *
 * Field order in the loader mirrors CTFAK 2.0's readers exactly:
 *   MFA/MFAData.cs, MFA/MFAFrame.cs, MFA/MFAObjectInfo.cs,
 *   MFA/MFAObjectLoaders/ .cs, CCN/Chunks/Banks/ .cs,
 *   CCN/Chunks/Frame/Events/Events.cs, CCN/Chunks/Frame/Events/Expression.cs
 */
#ifndef FXP_MFA_H
#define FXP_MFA_H

#include "fxp.h"
#include "byte_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ banks */

typedef struct fxp_image {
    int32_t      handle;
    int32_t      checksum;
    int32_t      references;
    int16_t      width, height;
    uint8_t      graphic_mode;
    uint8_t      flags;               /* FXP_IMGFLAG_*                    */
    int16_t      hotspot_x, hotspot_y;
    int16_t      action_x, action_y;
    fxp_color_t  transparent;
    uint8_t     *rgba;                /* decoded RGBA8888 (owned)         */
    size_t       rgba_size;
    /* 1bpp collision mask built from the alpha channel (alpha > 128),
     * MSB-first bits, row stride (width+7)/8 — NuclearRT
     * CollisionMaskBuilder layout.  NULL when the image has no alpha. */
    uint8_t     *col_mask;            /* owned                            */
    uint32_t     col_mask_stride;     /* bytes per row                    */
} fxp_image_t;

typedef struct fxp_image_bank {
    int32_t       graphic_mode;
    int16_t       palette_version;
    int16_t       palette_entries;
    fxp_color_t   palette[256];
    int32_t       count;
    fxp_image_t  *items;              /* owned                            */
} fxp_image_bank_t;

typedef struct fxp_sound {
    int32_t   handle;                 /* as read, minus 1 (CTFAK rule)    */
    int32_t   checksum;
    uint32_t  references;
    uint32_t  flags;
    char     *name;
    uint8_t  *data;
    int32_t   data_size;
} fxp_sound_t;

typedef struct fxp_sound_bank {
    int32_t       count;
    fxp_sound_t  *items;
} fxp_sound_bank_t;

/* "ASUM" music item: zlib-wrapped { checksum, refs, size, flags,
 * reserved, name, data } (MusicBank.cs MusicFile).  Data is raw OGG/MP3
 * — kept for the audio decoder. */
typedef struct fxp_music {
    int32_t   handle;
    char     *name;
    uint8_t  *data;
    int32_t   data_size;
} fxp_music_t;

typedef struct fxp_font {
    uint32_t    handle;               /* minus 1 when build > 284         */
    int32_t     checksum;
    int32_t     references;
    int32_t     height, width, escapement, orientation, weight;
    uint8_t     italic, underline, strike_out, char_set;
    uint8_t     out_precision, clip_precision, quality, pitch_and_family;
    char       *face_name;            /* UTF-8                            */
} fxp_font_t;

typedef struct fxp_font_bank {
    int32_t      count;
    fxp_font_t  *items;
} fxp_font_bank_t;

/* ----------------------------------------------------------------- values */

typedef struct fxp_value_item {
    char    *name;                    /* UTF-8                            */
    int32_t  type;                    /* FXP_VTYPE_*                      */
    int32_t  i;
    float    f;
    char    *s;                       /* UTF-8, owned (type == STRING)    */
} fxp_value_item_t;

typedef struct fxp_value_list {
    int32_t            count;
    fxp_value_item_t  *items;
} fxp_value_list_t;

/* ----------------------------------------------------------------- events */

typedef struct fxp_expr_item {
    int16_t  object_type;             /* -1 system, 0/0 terminator, else obj */
    int16_t  num;
    int32_t  int_val;                 /* LONG / GLOBAL index              */
    double   dbl_val;                 /* DOUBLE                           */
    float    dbl_padding;
    char    *str_val;                 /* STRING (owned)                   */
    uint16_t object_info;             /* OBJECT items                     */
    int16_t  object_info_list;
    int16_t  ext_val;                 /* inline i16 (obj items num 16/19) */
} fxp_expr_item_t;

typedef struct fxp_expression {
    int32_t           comparison;     /* leading comparison word          */
    int32_t           count;
    fxp_expr_item_t  *items;          /* INFIX token stream, terminated
                                      * by {0,0} (source order)          */
} fxp_expression_t;

typedef struct fxp_param {
    int16_t  code;
    uint8_t *raw;                     /* full parameter payload (owned)   */
    int32_t  raw_size;
    /* Parsed views (valid when the kind matches): */
    int32_t  int_val;                 /* SHORT / INT / GLOBAL / ALTERABLE */
    char    *str_val;                 /* STRING (owned)                   */
    bool     has_expression;
    fxp_expression_t expression;
} fxp_param_t;

typedef struct fxp_condition {
    int16_t  object_type;
    int16_t  num;
    uint16_t object_info;
    int16_t  object_info_list;
    int8_t   flags;
    int8_t   other_flags;             /* bit0 = negated                   */
    uint8_t  def_type;
    int16_t  identifier;
    int32_t  param_count;
    fxp_param_t *params;
} fxp_condition_t;

typedef struct fxp_action {
    int16_t  object_type;
    int16_t  num;
    uint16_t object_info;
    int16_t  object_info_list;
    int8_t   flags;
    int8_t   other_flags;
    uint8_t  def_type;
    int32_t  param_count;
    fxp_param_t *params;
} fxp_action_t;

typedef struct fxp_event_group {
    uint8_t  condition_count;
    uint8_t  action_count;
    uint16_t flags;
    int16_t  is_restricted;           /* MFA: 16-bit fields               */
    int16_t  restrict_cpt;
    int16_t  identifier;
    int16_t  undo;
    fxp_condition_t *conditions;
    fxp_action_t    *actions;
} fxp_event_group_t;

/* "EvOb" rows (MFAEvents.cs EventObject): the event editor's object
 * columns.  ObjectType 3 rows are QUALIFIER references — their Handle is
 * the pseudo object-info used by conditions/actions/expressions, and
 * SystemQualifier/ItemType identify the qualified group. */
typedef struct fxp_event_object {
    int32_t  handle;
    uint16_t object_type;             /* 1 frame item, 3 system qualifier */
    uint16_t item_type;               /* Fusion object type               */
    uint32_t item_handle;             /* ObjectType == 1                  */
    uint16_t system_qualifier;        /* ObjectType == 3                  */
} fxp_event_object_t;

typedef struct fxp_events {
    uint16_t version;
    uint16_t frame_type;
    int32_t  group_count;
    fxp_event_group_t *groups;
    int32_t  object_count;            /* EvOb rows                        */
    fxp_event_object_t *objects;
} fxp_events_t;

/* Translate every object reference inside `ev` (condition/action
 * object_info, object-carrying parameter payloads, expression items)
 * from event-editor handle space ("EvOb" row handles) to frame item
 * handle space.  Called automatically after fxp_events_read; public so
 * tools and tests can re-run or inspect it. */
void fxp_events_translate_objectinfos(fxp_events_t *ev);

/* ------------------------------------------------------------------ items */

typedef struct fxp_anim_dir {
    int32_t  index, min_speed, max_speed, repeat, back_to;
    int32_t  frame_count;
    int32_t *frames;
} fxp_anim_dir_t;

typedef struct fxp_anim {
    char            *name;
    int32_t          dir_count;
    fxp_anim_dir_t  *dirs;
} fxp_anim_t;

typedef struct fxp_movement {
    char    *name;                    /* UTF-8                            */
    char    *extension;               /* empty for built-ins              */
    uint32_t identifier;
    uint16_t player;
    uint16_t type;                    /* FXP_MOV_*                        */
    uint8_t  moving_at_start;
    int32_t  direction_at_start;
    /* parsed loader params (mfa_objects.c), 0 when the type has none */
    int32_t  mv_speed;                /* ball / race / 8dir / platform    */
    int32_t  mv_acceleration;         /* race / 8dir / platform           */
    int32_t  mv_deceleration;         /* ball / race / 8dir / platform    */
    int32_t  mv_randomizer;           /* ball                             */
    int32_t  mv_angles;               /* ball: 4/8/16/32 allowed angles   */
    int32_t  mv_security;             /* ball                             */
    int32_t  mv_bounce_factor;        /* race / 8dir                      */
    int32_t  mv_directions;           /* 8dir: 32-bit direction mask      */
    uint8_t *data;                    /* remaining loader bytes (owned)   */
    int32_t  data_size;
} fxp_movement_t;

typedef struct fxp_paragraph {
    char    *text;                    /* UTF-8                            */
    uint32_t flags;
} fxp_paragraph_t;

typedef struct fxp_item fxp_item_t;

/* Per-type payload.  `common` is shared by every object type. */
typedef struct fxp_item_common {
    int32_t          object_flags;
    int32_t          new_object_flags;
    fxp_color_t      back_color;
    int16_t          qualifiers[8];   /* -1 terminated in file            */
    fxp_value_list_t values;
    fxp_value_list_t strings;
    int32_t          movement_count;
    fxp_movement_t  *movements;
    int32_t          behaviour_count;
    char           **behaviour_names; /* + data blob pairs                */
    uint8_t        **behaviour_data;
    int32_t         *behaviour_sizes;
} fxp_item_common_t;

struct fxp_item {
    int32_t      object_type;
    int32_t      handle;
    char        *name;                /* UTF-8                            */
    int32_t      transparent;
    int32_t      ink_effect;
    uint32_t     ink_effect_param;
    int32_t      anti_aliasing;
    int32_t      flags;
    int32_t      icon_type;
    int32_t      icon_handle;

    fxp_item_common_t common;

    union {
        struct {                          /* BACKDROP                    */
            int32_t obstacle_type, collision_type;
            int32_t image;
        } backdrop;
        struct {                          /* QUICK_BACKDROP              */
            int32_t obstacle, collision, w, h;
            int32_t shape, border_size;
            fxp_color_t border_color;
            int32_t fill_type;
            fxp_color_t color1, color2;
            int32_t flags, image;
        } quick_backdrop;
        struct {                          /* ACTIVE                      */
            uint8_t  has_anims;
            int32_t  anim_count;
            fxp_anim_t *anims;
        } active;
        struct {                          /* TEXT                        */
            int32_t  w, h, font;
            fxp_color_t color;
            uint32_t flags;
            int32_t  paragraph_count;
            fxp_paragraph_t *paragraphs;
        } text;
        struct {                          /* COUNTER                     */
            int32_t  value, min, max;
            uint32_t display_type, count_flags;
            fxp_color_t color1, color2;
            int32_t  vertical_gradient, count_type, w, h;
            int32_t  image_count;
            int32_t *images;
            int32_t  font;
        } counter;
        struct {                          /* SCORE (5) / LIVES (6)       */
            int32_t  player;
            int32_t  image_count;
            int32_t *images;
            int32_t  display_type, flags, font, w, h;
        } lives;
        struct {                          /* SUBAPPLICATION (9)          */
            char    *file_name;
            int32_t  w, h, flag, frame_num;
        } subapp;
        struct {                          /* EXTENSION (>= 32)           */
            int32_t  ext_type;        /* -1                               */
            char    *ext_name, *ext_filename, *ext_sub_type;
            int32_t  magic;
            uint32_t ext_id, ext_version, ext_private;
            uint8_t *data;
            int32_t  data_size;
        } extension;
    } as;
};

/* ------------------------------------------------------------------ frame */

typedef struct fxp_mfa_layer {
    char    *name;
    uint32_t flags;
    float    x_coefficient, y_coefficient;
} fxp_mfa_layer_t;

typedef struct fxp_mfa_transition {
    char       *module, *name;
    char        id[5], transition_id[5];
    int32_t     duration, flags;
    fxp_color_t color;
    uint8_t    *params;
    int32_t     params_size;
} fxp_mfa_transition_t;

typedef struct fxp_mfa_instance {
    int32_t  x, y;
    uint32_t layer;
    int32_t  handle;
    int16_t  flags;
    int16_t  instance;
    uint32_t parent_type;
    uint32_t item_handle;
    int32_t  parent_handle;
} fxp_mfa_instance_t;

typedef struct fxp_mfa_chunk {
    uint8_t  id;
    uint8_t *data;
    int32_t  size;
} fxp_mfa_chunk_t;

typedef struct fxp_mfa_frame {
    int32_t      handle;
    char        *name;
    int32_t      size_x, size_y;
    fxp_color_t  background;
    uint32_t     flags;
    int32_t      max_objects;
    int32_t      last_viewed_x, last_viewed_y;
    fxp_color_t  palette[256];
    int32_t      stamp_handle, active_layer;
    int32_t      layer_count;
    fxp_mfa_layer_t *layers;
    uint8_t      has_fade_in, has_fade_out;
    fxp_mfa_transition_t fade_in, fade_out;
    int32_t      item_count;
    fxp_item_t **items;               /* owned                            */
    int32_t      instance_count;
    fxp_mfa_instance_t *instances;
    fxp_events_t events;
    int32_t      chunk_count;
    fxp_mfa_chunk_t *chunks;          /* app/frame mini-chunks            */
} fxp_mfa_frame_t;

/* -------------------------------------------------------------------- app */

typedef struct fxp_ext_entry {
    int32_t  handle;
    char    *name, *filename, *sub_type;
    int32_t  magic;
} fxp_ext_entry_t;

typedef struct fxp_qual_entry {
    char   *name;
    int32_t handle;
} fxp_qual_entry_t;

typedef struct fxp_player_control {
    int32_t control_type;
    int32_t keys[16];                 /* Up,Down,Left,Right,Btn1-4,Unk1-8 */
} fxp_player_control_t;

typedef struct fxp_binary_file {
    char    *name;
    uint8_t *data;
    int32_t  size;
} fxp_binary_file_t;

typedef struct fxp_mfa {
    int16_t  version, subversion;
    int32_t  product, build, lang_id;
    char    *name, *description, *path;

    fxp_font_bank_t   fonts;
    fxp_sound_bank_t  sounds;
    int32_t           music_count;     /* ASUM items (decoded on demand)  */
    fxp_music_t      *music_items;
    fxp_image_bank_t  icons;
    fxp_image_bank_t  images;

    char    *author, *copyright, *company, *version_str;
    int32_t  window_x, window_y;
    fxp_color_t border_color;
    uint32_t display_flags, graphic_flags;
    char    *help_file;
    int32_t  initial_score, initial_lives, frame_rate, build_type;
    char    *build_path, *command_line, *aboutbox;

    int32_t  binary_count;
    fxp_binary_file_t *binaries;

    int32_t  control_count;
    fxp_player_control_t *controls;

    fxp_value_list_t global_values;
    fxp_value_list_t global_strings;
    uint8_t *global_events;
    int32_t  global_events_size;
    int32_t  graphic_mode;

    int32_t  icon_count;
    int32_t *icon_images;

    int32_t  qual_count;
    fxp_qual_entry_t *quals;

    int32_t  ext_count;
    fxp_ext_entry_t *extensions;

    int32_t       frame_count;
    fxp_mfa_frame_t **frames;

    int32_t          chunk_count;
    fxp_mfa_chunk_t *chunks;
} fxp_mfa_t;

/* ---------------------------------------------------------------- loader */

/* Load an MFA from a memory buffer.  Returns NULL on failure (message is
 * logged).  `size` must be the exact buffer length. */
fxp_mfa_t *fxp_mfa_load(const void *data, size_t size);
void       fxp_mfa_free(fxp_mfa_t *mfa);

/* Locate a frame item (object info) by handle inside a frame. */
fxp_item_t *fxp_mfa_frame_item(const fxp_mfa_frame_t *frame, int32_t handle);

#ifdef __cplusplus
}
#endif
#endif /* FXP_MFA_H */
