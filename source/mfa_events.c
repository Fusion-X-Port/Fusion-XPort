/*
 * Fusion X-Port — mfa_events.c
 * MFA event parser: "Evts" groups, conditions, actions, parameters and
 * RPN expressions.  Layouts match CTFAK's Events.cs / Expression.cs:
 *
 *   EventGroup:  i16 size (NEGATIVE in MFA, total incl. this field; after
 *                the group, seek to groupStart + |size|),
 *                u8 nCond, u8 nAct, u16 flags,
 *                i16 isRestricted, i16 restrictCpt, i16 identifier, i16 undo,
 *                conditions, actions
 *   Condition:   u16 size (total incl. field), i16 objType, i16 num,
 *                u16 objInfo, i16 objInfoList, i8 flags, i8 otherFlags,
 *                u8 nParams, u8 defType, i16 identifier, params
 *   Action:      same as condition minus identifier
 *   Parameter:   i16 size (total incl. field), i16 code, payload
 *   Expression:  i16 objType, i16 num [terminator {0,0}], i16 size (total
 *                incl. field), type payload
 */
#include "mfa_internal.h"
#include "fxp/mfa_format.h"

/* ------------------------------------------------------------- expression */

static void expr_item_free(fxp_expr_item_t *it)
{
    free(it->str_val);
    it->str_val = NULL;
}

static void expression_free(fxp_expression_t *e)
{
    for (int32_t i = 0; i < e->count; i++)
        expr_item_free(&e->items[i]);
    free(e->items);
    e->items = NULL;
    e->count = 0;
}

/* Read one expression item.  Returns false at hard EOF; terminator is
 * signalled by *out_kind == 0.
 * NOTE: Expression.Write makes `size` the total item length INCLUDING the
 * type/num/size fields themselves (newWriter.Size() + 6), and Read seeks
 * to itemStart + size. */
static bool expr_item_read(fxp_reader_t *r, fxp_expr_item_t *it, int *out_kind)
{
    memset(it, 0, sizeof(*it));
    size_t item_start = fxp_r_tell(r);
    it->object_type = fxp_r_i16(r);
    it->num         = fxp_r_i16(r);

    if (it->object_type == 0 && it->num == 0) {   /* terminator */
        *out_kind = 0;
        return true;
    }

    int16_t size = fxp_r_i16(r);

    if (it->object_type == FXP_OT_SYSTEM) {
        switch (it->num) {
        case FXP_EXPR_LONG:
            it->int_val = fxp_r_i32(r);
            break;
        case FXP_EXPR_STRING:
            it->str_val = fxp_r_yuniversal(r, -1);
            break;
        case FXP_EXPR_DOUBLE:
            it->dbl_val     = fxp_r_f64(r);
            it->dbl_padding = fxp_r_f32(r);
            break;
        case 24:
        case 50:
            (void)fxp_r_i32(r);           /* always zero (GlobalCommon) */
            it->int_val = fxp_r_i32(r);   /* global value/string index  */
            break;
        default:
            break;                        /* function call: num is the op */
        }
    } else if (it->object_type >= 2 || it->object_type == FXP_OT_PLAYER) {
        it->object_info      = fxp_r_u16(r);
        it->object_info_list = fxp_r_i16(r);
        if (it->num == FXP_EXPR_EXT || it->num == 19)
            it->ext_val = fxp_r_i16(r);   /* ExtensionExp / inline index */
        else
            (void)fxp_r_i32(r);           /* unknown dword (Expression.cs) */
    }

    *out_kind = 1;

    /* Honour the size field exactly (Expression.cs seeks start+size). */
    if (size >= 6)
        fxp_r_seek(r, item_start + (size_t)size);
    return fxp_r_ok(r);
}

static bool expression_read(fxp_reader_t *r, fxp_expression_t *out)
{
    memset(out, 0, sizeof(*out));
    out->comparison = fxp_r_i16(r);

    int32_t cap = 8;
    out->items = fxp_malloc((size_t)cap * sizeof(fxp_expr_item_t));

    for (;;) {
        if (out->count == cap) {
            cap *= 2;
            out->items = fxp_realloc(out->items, (size_t)cap * sizeof(fxp_expr_item_t));
        }
        int kind = 0;
        if (!expr_item_read(r, &out->items[out->count], &kind)) {
            expression_free(out);
            return false;
        }
        if (kind == 0)
            break;                        /* {0,0} terminator (kept in list) */
        out->count++;
        if (out->count > 65536) {         /* paranoia for malformed files */
            expression_free(out);
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- parameters */

static void param_free(fxp_param_t *p)
{
    free(p->raw);
    free(p->str_val);
    if (p->has_expression)
        expression_free(&p->expression);
    memset(p, 0, sizeof(*p));
}

static bool param_read(fxp_reader_t *r, fxp_param_t *p)
{
    memset(p, 0, sizeof(*p));
    size_t start = fxp_r_tell(r);
    int16_t size = fxp_r_i16(r);
    p->code      = fxp_r_i16(r);

    switch (p->code) {
    case FXP_PARAM_SHORT:
    case 4: case 10: case 11: case 12: case 17: case 26: case 31:
    case 43: case 57: case 58: case 60: case 61:
    case FXP_PARAM_INT:
    case 25: case 29: case 34: case 48: case 56: case 67: case 70:
    case FXP_PARAM_GLOBALVALUE:
    case FXP_PARAM_ALTERABLE: {
        /* Short params are 16-bit; Int/Global/Alterable params are 32-bit. */
        int32_t remaining = size - 4;
        if (remaining == 2)
            p->int_val = fxp_r_i16(r);
        else
            p->int_val = fxp_r_i32(r);
        break;
    }
    case FXP_PARAM_STRING:
    case 64:
        p->str_val = fxp_r_yuniversal(r, -1);
        break;
    case FXP_PARAM_EXPRESSION:
    case 22: case 23: case 27: case 28:
    case 45: case 46: case 52: case 53: case 54: case 59: case 62:
        p->has_expression = true;
        if (!expression_read(r, &p->expression))
            return false;
        break;
    default:
        break;                            /* retained raw for the VM */
    }

    /* keep the full payload for the interpreter (Parameters.Read seeks to
     * start+size, so unknown codes survive round-trips) */
    if (size >= 4 && start + (size_t)size <= fxp_r_size(r)) {
        p->raw_size = size;
        p->raw = fxp_malloc((size_t)size);
        memcpy(p->raw, r->base + start, (size_t)size);
        fxp_r_seek(r, start + (size_t)size);
    } else {
        fxp_log_warn("parameter code %d: bad size %d\n", p->code, size);
        fxp_r_seek(r, fxp_r_size(r));
        return false;
    }
    return true;
}

/* ------------------------------------------------------ conditions/actions */

static void condition_free(fxp_condition_t *c)
{
    for (int32_t i = 0; i < c->param_count; i++)
        param_free(&c->params[i]);
    free(c->params);
}

static bool condition_read(fxp_reader_t *r, fxp_condition_t *c)
{
    memset(c, 0, sizeof(*c));
    size_t cond_start = fxp_r_tell(r);
    int16_t size = fxp_r_i16(r);         /* total length incl. itself       */
    c->object_type     = fxp_r_i16(r);
    c->num             = fxp_r_i16(r);
    c->object_info     = fxp_r_u16(r);
    c->object_info_list = fxp_r_i16(r);
    c->flags           = fxp_r_i8(r);
    c->other_flags     = fxp_r_i8(r);
    uint8_t nparams    = fxp_r_u8(r);
    c->def_type        = fxp_r_u8(r);
    c->identifier      = fxp_r_i16(r);

    c->params = fxp_calloc((size_t)(nparams ? nparams : 1), sizeof(fxp_param_t));
    for (int i = 0; i < nparams; i++) {
        if (!param_read(r, &c->params[c->param_count])) {
            condition_free(c);
            return false;
        }
        c->param_count++;
    }
    /* trust the size field over param drift (mirrors Condition.Write) */
    if (size >= 12 && cond_start + (size_t)size <= fxp_r_size(r))
        fxp_r_seek(r, cond_start + (size_t)size);
    return fxp_r_ok(r);
}

static void action_free(fxp_action_t *a)
{
    for (int32_t i = 0; i < a->param_count; i++)
        param_free(&a->params[i]);
    free(a->params);
}

static bool action_read(fxp_reader_t *r, fxp_action_t *a)
{
    memset(a, 0, sizeof(*a));
    size_t act_start = fxp_r_tell(r);
    int16_t size = fxp_r_i16(r);         /* total length incl. itself       */
    a->object_type     = fxp_r_i16(r);
    a->num             = fxp_r_i16(r);
    a->object_info     = fxp_r_u16(r);
    a->object_info_list = fxp_r_i16(r);
    a->flags           = fxp_r_i8(r);
    a->other_flags     = fxp_r_i8(r);
    uint8_t nparams    = fxp_r_u8(r);
    a->def_type        = fxp_r_u8(r);

    a->params = fxp_calloc((size_t)(nparams ? nparams : 1), sizeof(fxp_param_t));
    for (int i = 0; i < nparams; i++) {
        if (!param_read(r, &a->params[a->param_count])) {
            action_free(a);
            return false;
        }
        a->param_count++;
    }
    if (size >= 12 && act_start + (size_t)size <= fxp_r_size(r))
        fxp_r_seek(r, act_start + (size_t)size);
    return fxp_r_ok(r);
}

/* ------------------------------------------------------------------ groups */

static void group_free(fxp_event_group_t *g)
{
    for (int32_t i = 0; i < g->condition_count; i++)
        condition_free(&g->conditions[i]);
    for (int32_t i = 0; i < g->action_count; i++)
        action_free(&g->actions[i]);
    free(g->conditions);
    free(g->actions);
}

static bool group_read(fxp_reader_t *r, fxp_event_group_t *g)
{
    memset(g, 0, sizeof(*g));
    size_t group_start = fxp_r_tell(r);
    int16_t size       = fxp_r_i16(r);

    uint8_t ncond = fxp_r_u8(r);
    uint8_t nact  = fxp_r_u8(r);
    g->flags           = fxp_r_u16(r);
    /* MFA variant (Events.cs: isMFA -> four 16-bit fields) */
    g->is_restricted   = fxp_r_i16(r);
    g->restrict_cpt    = fxp_r_i16(r);
    g->identifier      = fxp_r_i16(r);
    g->undo            = fxp_r_i16(r);

    g->conditions = fxp_calloc((size_t)(ncond ? ncond : 1), sizeof(fxp_condition_t));
    for (int i = 0; i < ncond; i++) {
        if (!condition_read(r, &g->conditions[g->condition_count])) {
            group_free(g);
            return false;
        }
        g->condition_count++;
    }

    g->actions = fxp_calloc((size_t)(nact ? nact : 1), sizeof(fxp_action_t));
    for (int i = 0; i < nact; i++) {
        if (!action_read(r, &g->actions[g->action_count])) {
            group_free(g);
            return false;
        }
        g->action_count++;
    }

    /* MFA: size is negative and includes the i16 size field itself
     * (EventGroup.Read: reader.Seek(currentPosition + Size * -1)). */
    if (size < 0) {
        fxp_r_seek(r, group_start + (size_t)(-size));
    } else if (size > 0) {
        fxp_r_seek(r, group_start + (size_t)size);
    }
    return fxp_r_ok(r);
}

/* ------------------------------------------------------------------ blocks */

/* EventObject (MFAEvents.cs) — editor rows in the event editor.  Retained
 * because ObjectType == 3 rows carry the qualifier pseudo-handles that
 * conditions/actions/expressions reference. */
static bool event_object_read(fxp_reader_t *r, fxp_event_object_t *out)
{
    memset(out, 0, sizeof(*out));
    out->handle      = (int32_t)fxp_r_u32(r);
    out->object_type = fxp_r_u16(r);  /* ObjectType   */
    out->item_type   = fxp_r_u16(r);  /* ItemType     */
    free(fxp_r_auto_unicode(r));      /* Name         */
    free(fxp_r_auto_unicode(r));      /* TypeName     */
    (void)fxp_r_u16(r);               /* Flags        */
    if (out->object_type == 1) {      /* FrameItemType */
        out->item_handle = fxp_r_u32(r);
        (void)fxp_r_u32(r);           /* InstanceHandle */
    } else if (out->object_type == 2) {   /* ShortcutItemType */
        char code[5];
        if (fxp_r_ascii4(r, code) && strcmp(code, "OIC2") == 0)
            free(fxp_r_auto_unicode(r));  /* IconBuffer    */
    }
    if (out->object_type == 3)        /* SystemItemType */
        out->system_qualifier = fxp_r_u16(r);
    return fxp_r_ok(r);
}

/* Comment (MFAEvents.cs): u32 Handle + AutoUnicode Value. */
static void comment_skip(fxp_reader_t *r)
{
    (void)fxp_r_u32(r);
    free(fxp_r_auto_unicode(r));
}

bool fxp_events_read(fxp_reader_t *r, fxp_events_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version    = fxp_r_u16(r);
    out->frame_type = fxp_r_u16(r);

    int32_t cap = 16;
    out->groups = fxp_malloc((size_t)cap * sizeof(fxp_event_group_t));

    char tag[5];
    for (;;) {
        if (!fxp_r_ascii4(r, tag))
            goto fail;
        if (strcmp(tag, FXP_EVT_END) == 0)
            break;

        if (strcmp(tag, FXP_EVT_DATA) == 0) {
            uint32_t len = fxp_r_u32(r);
            size_t end = fxp_r_tell(r) + len;
            while (fxp_r_tell(r) < end && fxp_r_ok(r)) {
                if (out->group_count == cap) {
                    cap *= 2;
                    out->groups = fxp_realloc(out->groups,
                                              (size_t)cap * sizeof(fxp_event_group_t));
                }
                if (!group_read(r, &out->groups[out->group_count]))
                    goto fail;
                out->group_count++;
            }
        } else if (strcmp(tag, FXP_EVT_COMMENTS) == 0) {
            /* CTFAK: CommentDataLen = u32, then ONE Comment record. */
            (void)fxp_r_u32(r);
            comment_skip(r);
        } else if (strcmp(tag, FXP_EVT_OBJECTS) == 0) {
            /* u32 COUNT, then that many variable-length EventObject
             * records — NOT a byte length (MFAEvents.cs ObjectData). */
            uint32_t count = fxp_r_u32(r);
            if (count > 1048576)
                goto fail;
            out->object_count = (int32_t)count;
            out->objects = fxp_calloc((size_t)(count ? count : 1),
                                      sizeof(fxp_event_object_t));
            for (uint32_t i = 0; i < count; i++) {
                if (!event_object_read(r, &out->objects[i]))
                    goto fail;
            }
        } else if (strcmp(tag, FXP_EVT_EDITOR) == 0) {
            (void)fxp_r_i32(r);           /* EditorDataUnk */
            (void)fxp_r_u16(r);           /* ConditionWidth */
            (void)fxp_r_u16(r);           /* ObjectHeight  */
            fxp_r_skip(r, 12);
        } else if (strcmp(tag, FXP_EVT_OBJLIST) == 0) {
            int16_t count = fxp_r_i16(r);
            int32_t real = count;
            if (count == -1)
                real = fxp_r_i16(r);
            if (real < 0 || real > 1048576)
                goto fail;
            for (int32_t i = 0; i < real; i++)
                (void)fxp_r_u16(r);       /* ObjectTypes  */
            for (int32_t i = 0; i < real; i++)
                (void)fxp_r_u16(r);       /* ObjectHandles */
            for (int32_t i = 0; i < real; i++)
                (void)fxp_r_u16(r);       /* ObjectFlags  */
            if (count == -1) {
                uint16_t folders = fxp_r_u16(r);
                for (uint16_t i = 0; i < folders; i++)
                    free(fxp_r_auto_unicode(r));
            }
        } else if (strcmp(tag, FXP_EVT_POSITION) == 0) {
            /* version u16; X only read when version != 1 (MFAEvents.cs);
             * Y, CaretType, CaretX, CaretY always follow. */
            uint16_t ver = fxp_r_u16(r);
            if (ver != 1)
                (void)fxp_r_u32(r);       /* X */
            (void)fxp_r_u32(r);           /* Y */
            (void)fxp_r_u32(r);           /* CaretType */
            (void)fxp_r_u32(r);           /* CaretX */
            (void)fxp_r_u32(r);           /* CaretY */
        } else if (strcmp(tag, FXP_EVT_LINE) == 0) {
            uint16_t ver = fxp_r_u16(r);
            if (ver != 1)
                (void)fxp_r_u32(r);       /* LineY */
            (void)fxp_r_u32(r);           /* LineItemType */
            (void)fxp_r_u32(r);           /* EventLine */
            (void)fxp_r_u32(r);           /* EventLineY */
        } else if (strcmp(tag, FXP_EVT_UNKNOWN) == 0) {
            fxp_r_skip(r, 12);
        } else {
            fxp_log_warn("events: unknown block '%s' skipped", tag);
        }
    }

    fxp_events_translate_objectinfos(out);
    return true;

fail:
    fxp_events_free(out);
    return false;
}

/* ----------------------------------------------------- object info mapping */

/*
 * Event conditions/actions/expressions reference objects by the EVENT
 * EDITOR's row handle ("EvOb" EventObject.Handle), NOT directly by the
 * frame item handle.  Real Fusion files usually have sparse row handles
 * (objects deleted in the editor leave gaps: e.g. rows 0 and 2 mapping to
 * items 0 and 3), so every object reference must be translated through
 * the table.  CTFAK-written files and XPort test fixtures write identity
 * tables, making the translation a no-op there.
 *
 * Rows with object_type != 1 (qualifiers, system rows) are left alone:
 * their handles are the qualifier pseudo-handles the runtime resolves
 * through dedicated selectors.
 */
static int32_t events_translate_oi(const fxp_events_t *ev, int32_t oi)
{
    if (oi < 0 || !ev->objects)
        return oi;
    for (int32_t i = 0; i < ev->object_count; i++) {
        const fxp_event_object_t *eo = &ev->objects[i];
        if (eo->object_type == 1 && (int32_t)eo->handle == oi)
            return (int32_t)eo->item_handle;
    }
    return oi;
}

/* Payload readers: the payload starts after the [size i16][code i16]
 * parameter header (raw+4). */
static int32_t param_payload_u16(const fxp_param_t *p, int32_t offset)
{
    const uint8_t *d = p->raw ? p->raw + 4 : NULL;
    if (!d || p->raw_size < offset + 6)
        return 0;
    return (int32_t)(d[offset] | (d[offset + 1] << 8));
}

/* Patch a payload u16 at `offset` (relative to the payload, i.e. past the
 * [size i16][code i16] parameter header). */
static void param_patch_u16(fxp_param_t *p, int32_t offset, int32_t value)
{
    if (!p->raw || p->raw_size < offset + 6)
        return;
    p->raw[4 + offset]     = (uint8_t)(value & 0xFF);
    p->raw[4 + offset + 1] = (uint8_t)((value >> 8) & 0xFF);
}

static void events_translate_params(fxp_events_t *ev, fxp_param_t *params,
                                    int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        fxp_param_t *p = &params[i];
        if (p->has_expression) {
            fxp_expression_t *e = &p->expression;
            for (int32_t k = 0; k < e->count; k++) {
                fxp_expr_item_t *it = &e->items[k];
                if (it->object_type > 0)
                    it->object_info = (uint16_t)events_translate_oi(
                        ev, (int32_t)it->object_info);
            }
            continue;
        }
        switch (p->code) {
        case FXP_PARAM_OBJECT:            /* ParamObject: oi @2           */
            param_patch_u16(p, 2, events_translate_oi(
                ev, param_payload_u16(p, 2)));
            break;
        case FXP_PARAM_POSITION: {        /* Position: parent @0          */
            int32_t parent = param_payload_u16(p, 0);
            if (parent != 0xFFFF)
                param_patch_u16(p, 0,
                                events_translate_oi(ev, parent));
            break;
        }
        case FXP_PARAM_CREATE:            /* Create: Position + oi @26    */
        case FXP_PARAM_SHOOT: {           /* Shoot:  Position + oi @26    */
            int32_t parent = param_payload_u16(p, 0);
            if (parent != 0xFFFF)
                param_patch_u16(p, 0, events_translate_oi(ev, parent));
            param_patch_u16(p, 26, events_translate_oi(
                ev, param_payload_u16(p, 26)));
            break;
        }
        default:
            break;
        }
    }
}

void fxp_events_translate_objectinfos(fxp_events_t *ev)
{
    if (!ev || ev->object_count == 0)
        return;
    for (int32_t g = 0; g < ev->group_count; g++) {
        fxp_event_group_t *gr = &ev->groups[g];
        for (int32_t c = 0; c < gr->condition_count; c++) {
            fxp_condition_t *cd = &gr->conditions[c];
            if (cd->object_type > 0)
                cd->object_info = (uint16_t)events_translate_oi(
                    ev, (int32_t)cd->object_info);
            events_translate_params(ev, cd->params, cd->param_count);
        }
        for (int32_t a = 0; a < gr->action_count; a++) {
            fxp_action_t *ac = &gr->actions[a];
            if (ac->object_type > 0)
                ac->object_info = (uint16_t)events_translate_oi(
                    ev, (int32_t)ac->object_info);
            events_translate_params(ev, ac->params, ac->param_count);
        }
    }
}

void fxp_events_free(fxp_events_t *ev)
{
    for (int32_t i = 0; i < ev->group_count; i++)
        group_free(&ev->groups[i]);
    free(ev->groups);
    ev->groups = NULL;
    ev->group_count = 0;
    free(ev->objects);
    ev->objects = NULL;
    ev->object_count = 0;
}
