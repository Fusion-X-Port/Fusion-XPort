/*
 * Fusion X-Port — mfa_format.h
 * Binary format constants for Clickteam Fusion MFA (and CCN) files.
 *
 * Every constant below was verified against CTFAK 2.0 sources
 * (Core/CTFAK.Core).  MFA files are the editor format: unencrypted,
 * un-compressed chunk framing — which is exactly what Fusion X-Port
 * executes, by design, instead of compiled EXE/CCN distributions.
 */
#ifndef FXP_MFA_FORMAT_H
#define FXP_MFA_FORMAT_H

/* MFA magic (MFAData.cs:299 — read via ReadAscii(4), written as "MFU2") */
#define FXP_MFA_MAGIC "MFU2"

/* Bank tags (MFAData.cs:316-335) */
#define FXP_BANK_FONTS  "ATNF"
#define FXP_BANK_SOUNDS "APMS"
#define FXP_BANK_MUSIC  "ASUM"
#define FXP_BANK_IMAGES "AGMI"   /* used by both the icon and the image bank */

/* MFA mini-chunk framing (MFAChunk.Read): u8 id, 0 terminates the stream.
 * Only the ids below have defined meaning (MFAChunks.cs GetOrCreateChunk). */
enum {
    FXP_MFA_CHUNK_FRAME_VIRTUAL_RECT   = 33,
    FXP_MFA_CHUNK_LAYER_SHADER         = 37,
    FXP_MFA_CHUNK_FRAME_MOVEMENT_TIMER = 39,
    FXP_MFA_CHUNK_FRAME_SHADER         = 40,
    FXP_MFA_CHUNK_OBJECT_SHADER        = 45,
};

/* Fusion object type identifiers (Constants.cs:29-50) */
enum {
    FXP_OT_PLAYER         = -7,
    FXP_OT_KEYBOARD       = -6,
    FXP_OT_CREATE         = -5,
    FXP_OT_TIMER          = -4,
    FXP_OT_GAME           = -3,
    FXP_OT_SPEAKER        = -2,
    FXP_OT_SYSTEM         = -1,
    FXP_OT_QUICK_BACKDROP = 0,
    FXP_OT_BACKDROP       = 1,
    FXP_OT_ACTIVE         = 2,
    FXP_OT_TEXT           = 3,
    FXP_OT_QUESTION       = 4,
    FXP_OT_SCORE          = 5,
    FXP_OT_LIVES          = 6,
    FXP_OT_COUNTER        = 7,
    FXP_OT_RTF            = 8,
    FXP_OT_SUBAPPLICATION = 9,
    FXP_OT_EXTENSION      = 32,  /* 32+handle identifies a concrete extension */
};

/* Movement types (Objects/Movements.cs type dispatch) */
enum {
    FXP_MOV_MOUSE           = 1,
    FXP_MOV_RACE            = 2,
    FXP_MOV_EIGHT_DIRECTIONS= 3,
    FXP_MOV_BOUNCING_BALL   = 4,
    FXP_MOV_PATH            = 5,
    FXP_MOV_PLATFORM        = 9,
    FXP_MOV_EXTENSION       = 14,
};

/* Image flags (FusionImage.cs:48-58, bit indices) */
enum {
    FXP_IMGFLAG_RLE  = 1 << 0,
    FXP_IMGFLAG_RLEW = 1 << 1,
    FXP_IMGFLAG_RLET = 1 << 2,
    FXP_IMGFLAG_LZX  = 1 << 3,   /* zlib-compressed payload               */
    FXP_IMGFLAG_ALPHA= 1 << 4,   /* separate alpha plane present          */
    FXP_IMGFLAG_ACE  = 1 << 5,
    FXP_IMGFLAG_MAC  = 1 << 6,
    FXP_IMGFLAG_RGBA = 1 << 7,
};

/* Graphic modes (FusionImage.cs:14-31).  MFA files produced by CTFAK's
 * FTDecompile normalize everything to mode 4. */
enum {
    FXP_GM_ANDROID_0 = 0,
    FXP_GM_ANDROID_1 = 1,
    FXP_GM_ANDROID_2 = 2,
    FXP_GM_ANDROID_3 = 3,
    FXP_GM_ANDROID_4 = 4,
    FXP_GM_ANDROID_5 = 5,
    FXP_GM_24BIT     = 4,   /* R,G,B rows (2-byte pixel-unit padding) +
                               optional 4-byte-aligned 8-bit alpha plane */
    FXP_GM_15BIT     = 6,
    FXP_GM_16BIT     = 7,
    FXP_GM_32BIT     = 8,   /* 2.5+/Fusion3: 32bpp LZ4 (CCN) / zlib (MFA) */
};

/* Value item types (MFAValueList.cs ValueItem) */
enum {
    FXP_VTYPE_INT    = 0,
    FXP_VTYPE_FLOAT  = 1,
    FXP_VTYPE_STRING = 2,
};

/* MFA event block tags (MFAEvents.cs:13-22) */
#define FXP_EVT_DATA      "Evts"
#define FXP_EVT_COMMENTS  "Rems"
#define FXP_EVT_OBJECTS   "EvOb"
#define FXP_EVT_EDITOR    "EvCs"
#define FXP_EVT_OBJLIST   "EvEd"
#define FXP_EVT_POSITION  "EvTs"
#define FXP_EVT_LINE      "EvLs"
#define FXP_EVT_UNKNOWN   "E2Ts"
#define FXP_EVT_END       "!DNE"

/* Parameter codes (Events.cs LoadParameter dispatch, values used by the
 * scaffold parser; anything else is retained raw for later interpretation) */
enum {
    FXP_PARAM_TIME        = 2,   /* 2/42: i32, i32, i16 */
    FXP_PARAM_SHORT       = 3,   /* 3/4/10/11/12/17/26/31/43/57/58/60/61 */
    FXP_PARAM_INT         = 5,   /* 5/25/29/34/48/56/67/70 */
    FXP_PARAM_EXPRESSION  = 15,  /* 15/22/23/27/28/45/46/52/53/54/59/62 */
    FXP_PARAM_STRING      = 41,  /* 41/64 */
    FXP_PARAM_GLOBALVALUE = 49,
    FXP_PARAM_ALTERABLE   = 50,
    FXP_PARAM_EVERY       = 13,
    FXP_PARAM_CREATE      = 9,
    FXP_PARAM_POSITION    = 16,
    FXP_PARAM_SHOOT       = 18,
    FXP_PARAM_ZONE        = 19,
    FXP_PARAM_SAMPLE      = 6,   /* 6/7/35/36 */
    FXP_PARAM_KEY         = 14,  /* 14/44 */
    FXP_PARAM_CLICK       = 32,
    FXP_PARAM_COLOUR      = 24,
    FXP_PARAM_GROUP       = 38,
    FXP_PARAM_CHILDEVENT  = 69,
    FXP_PARAM_MULTIVARS   = 68,
    FXP_PARAM_OBJECT      = 1,
};

/* Expression item "opcodes" (Expression.cs).  ObjectType/Num pairs:
 * system (-1) items carry constants; object items reference an object info. */
enum {
    FXP_EXPR_LONG     = 0,   /* system: int32 literal            */
    FXP_EXPR_STRING   = 3,   /* system: string literal           */
    FXP_EXPR_DOUBLE   = 23,  /* system: double + float padding   */
    FXP_EXPR_GLOBAL   = 24,  /* system: i32 0 + i32 global index (also 50) */
    FXP_EXPR_EXT      = 16,  /* object: extension expression (also 19) */
};

/* CCN chunk ids — kept for reference/testing, not needed to run MFAs. */
enum {
    FXP_CCN_APP_HEADER   = 0x2223,
    FXP_CCN_FRAME        = 0x3333,
    FXP_CCN_FRAME_EVENTS = 0x333D,
    FXP_CCN_FRAME_ITEMS  = 0x2229,
    FXP_CCN_FRAME_ITEMS2 = 0x223F,
    FXP_CCN_IMAGE_BANK   = 0x6666,
    FXP_CCN_FONT_BANK    = 0x6667,
    FXP_CCN_SOUND_BANK   = 0x6668,
    FXP_CCN_MUSIC_BANK   = 0x6669,
    FXP_CCN_LAST         = 0x7F7F,
};

#endif /* FXP_MFA_FORMAT_H */
