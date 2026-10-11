/*
 * Fusion X-Port — event_runtime.c
 * The event interpreter: NuclearRT's event semantics as a C interpreter.
 *
 * Reference mapping (nuclearrt runtime/ + exporter/src):
 *   Frame::Update                     -> fxp_runtime_tick
 *   Frame::DispatchTrueEvents         -> dispatch_true_events
 *   GeneratedFrame::Update event loop -> run_event_pass
 *   EventProcessor.BuildEventFunctions-> exec_group
 *   ObjectSelector / ObjectIterator   -> fxp_selector_t iteration
 *   GameTimer::CheckEvent             -> timer_check_event
 *   Timer::ProcessNamedEvents         -> process_named_timers
 *   ExpressionConverter               -> fxp_expr_eval (RPN interpreter)
 *   CValue                            -> fxp_value_t
 *
 * Event execution rules (exporter EventProcessor + generated code):
 *   - skip rows with flags & 16384 (inactive), comment rows, group markers
 *   - per condition: reset the selector of every referenced object info
 *     (once per event), evaluate, negate via OtherFlags & 1
 *   - OR markers (-1,-25) split conditions into branches; the event
 *     passes if any branch fully passes (entry selections are
 *     snapshotted and restored between branches)
 *   - per action: reset selectors of object infos referenced for the
 *     first time, then iterate and execute
 */
#include "fxp/runtime.h"
#include "fxp/frame.h"
#include "fxp/mfa_format.h"
#include "fxp/backend.h"

#include <math.h>
#include <ctype.h>

enum { FXP_PASS_TIMER, FXP_PASS_NORMAL, FXP_PASS_EOF, FXP_PASS_LOOP,
       FXP_PASS_DISPATCH, FXP_PASS_NAMED };

/* Group classification (nuclearrt BuildEventUpdateLoop buckets). */
enum {
    FXP_GC_NORMAL = 0,
    FXP_GC_SOF,                       /* start of frame (true event)       */
    FXP_GC_EOF,                       /* end of frame                      */
    FXP_GC_TIMER,                     /* compare-to-timer (before normal)  */
    FXP_GC_NAMED,                     /* "on timer event" (named)          */
    FXP_GC_LOOP,                      /* "on loop" (fast loop)             */
    FXP_GC_TRUE,                      /* dispatched input true events      */
    FXP_GC_MARKER,                    /* group start/end rows              */
};

/* ============================================================ utilities == */

static uint32_t rt_rand(fxp_runtime_t *rt)
{
    /* xorshift64* — deterministic, console-friendly */
    uint64_t x = rt->rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rt->rng_state = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}

static int32_t rt_random(fxp_runtime_t *rt, int32_t max)
{
    /* Fusion Random(n): 0..n-1 (n <= 0 -> 0) */
    if (max <= 0)
        return 0;
    return (int32_t)(rt_rand(rt) % (uint32_t)max);
}

static void to_lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

static bool loop_name_equals(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* Warn once per (type, num) — unhandled conditions/actions fire every
 * tick, so a naive log call floods at frame rate (60 fps). */
typedef struct warn_key {
    int16_t type, num;
} warn_key_t;

static void warn_once(int32_t type, int32_t num, const char *what)
{
    static warn_key_t seen[64];
    static int32_t seen_count;
    for (int32_t i = 0; i < seen_count; i++)
        if (seen[i].type == (int16_t)type && seen[i].num == (int16_t)num)
            return;
    if (seen_count < 64) {
        seen[seen_count].type = (int16_t)type;
        seen[seen_count].num = (int16_t)num;
        seen_count++;
    }
    fxp_log_warn("events: unhandled %s (%d,%d) — suppressed from here on",
                 what, type, num);
}

bool fxp_compare(int32_t comparison, int cmp)
{
    /* Fusion comparison codes (ExpressionConverter.GetComparisonSymbol):
     * 0 ==, 1 !=, 2 <=, 3 <, 4 >=, 5 >  (cmp: <0, 0, >0) */
    switch (comparison) {
    case 0:  return cmp == 0;
    case 1:  return cmp != 0;
    case 2:  return cmp <= 0;
    case 3:  return cmp < 0;
    case 4:  return cmp >= 0;
    case 5:  return cmp > 0;
    default: return false;
    }
}

/* ==================================================== runtime lifecycle == */

fxp_runtime_t *fxp_runtime_create(const fxp_mfa_t *mfa, const fxp_mfa_frame_t *def)
{
    fxp_runtime_t *rt = fxp_calloc(1, sizeof(fxp_runtime_t));
    rt->mfa = mfa;
    rt->rng_state = 0x9E3779B97F4A7C15ULL ^ (uint64_t)(size_t)mfa;
    for (int32_t i = 0; i < 128; i++)
        rt->sample_channel[i] = -1;
    if (mfa) {
        /* AppData: every player starts with the MFA's initial score/lives
         * (MFA stores them plain; the XOR 0xffffffff encoding is CCN-only,
         * already decoded by CTFAK before an MFA is written). */
        int32_t iscore = mfa->initial_score > 0 ? mfa->initial_score : 0;
        int32_t ilives = mfa->initial_lives > 0 ? mfa->initial_lives : 0;
        for (int32_t i = 0; i < 4; i++) {
            rt->score[i] = iscore;
            rt->lives[i] = ilives;
        }
        rt->global_value_count = mfa->global_values.count;
        rt->global_values = fxp_calloc(
            (size_t)(mfa->global_values.count ? mfa->global_values.count : 1),
            sizeof(fxp_value_t));
        for (int32_t i = 0; i < mfa->global_values.count; i++) {
            const fxp_value_item_t *vi = &mfa->global_values.items[i];
            if (vi->type == FXP_VTYPE_FLOAT)
                fxp_value_init_double(&rt->global_values[i], vi->f);
            else
                fxp_value_init_int(&rt->global_values[i], vi->i);
        }
        rt->global_string_count = mfa->global_strings.count;
        rt->global_strings = fxp_calloc(
            (size_t)(mfa->global_strings.count ? mfa->global_strings.count : 1),
            sizeof(fxp_value_t));
        for (int32_t i = 0; i < mfa->global_strings.count; i++)
            fxp_value_init_string(&rt->global_strings[i],
                                  mfa->global_strings.items[i].s
                                      ? mfa->global_strings.items[i].s : "");
    }
    (void)def;
    return rt;
}

void fxp_runtime_free(fxp_runtime_t *rt)
{
    if (!rt)
        return;
    free(rt->timer_events);
    for (int32_t i = 0; i < rt->named_timer_count; i++)
        free(rt->named_timers[i].name);
    free(rt->named_timers);
    free(rt->current_named_event);
    for (int32_t i = 0; i < rt->loop_count; i++)
        free(rt->loops[i].name);
    free(rt->loops);
    free(rt->group_ids);
    free(rt->group_active);
    free(rt->run_once);
    free(rt->action_ran_last);
    free(rt->gclass);
    free(rt->gowner);
    for (int32_t i = 0; i < rt->global_value_count; i++)
        fxp_value_free(&rt->global_values[i]);
    free(rt->global_values);
    for (int32_t i = 0; i < rt->global_string_count; i++)
        fxp_value_free(&rt->global_strings[i]);
    free(rt->global_strings);
    free(rt);
}

/* ------------------------------------------------------- group registry -- */

static void group_state_ensure(fxp_frame_t *frame, int32_t group_count)
{
    fxp_runtime_t *rt = frame->rt;
    if (group_count <= (int32_t)rt->group_state_cap)
        return;
    uint32_t cap = rt->group_state_cap ? rt->group_state_cap : 64;
    while ((int32_t)cap < group_count)
        cap *= 2;
    rt->run_once        = fxp_realloc(rt->run_once, cap * sizeof(bool));
    rt->action_ran_last = fxp_realloc(rt->action_ran_last, cap * sizeof(bool));
    memset(rt->run_once + rt->group_state_cap, 0,
           (size_t)(cap - rt->group_state_cap) * sizeof(bool));
    memset(rt->action_ran_last + rt->group_state_cap, 0,
           (size_t)(cap - rt->group_state_cap) * sizeof(bool));
    rt->group_state_cap = cap;
}

static void group_ids_ensure(fxp_frame_t *frame, int32_t id)
{
    fxp_runtime_t *rt = frame->rt;
    if (id < (int32_t)rt->group_cap)
        return;
    int32_t cap = rt->group_cap ? rt->group_cap : 64;
    while (cap <= id)
        cap *= 2;
    rt->group_ids    = fxp_realloc(rt->group_ids, (size_t)cap * sizeof(int32_t));
    rt->group_active = fxp_realloc(rt->group_active, (size_t)cap * sizeof(bool));
    for (int32_t i = rt->group_cap; i < cap; i++) {
        rt->group_ids[i] = 0;
        rt->group_active[i] = true;
    }
    rt->group_cap = cap;
}

static bool group_is_active(fxp_frame_t *frame, int32_t id)
{
    if (id <= 0)
        return true;
    group_ids_ensure(frame, id);
    return frame->rt->group_active[id];
}

/* Group id of a GroupStart/GroupEnd marker row (param code 38:
 * [size i16][code i16][flags u16][id u16]...), else 0. */
static int32_t group_span_id(const fxp_event_group_t *g, bool *out_start,
                             bool *out_end)
{
    *out_start = *out_end = false;
    if (g->condition_count != 1)
        return 0;
    const fxp_condition_t *c = &g->conditions[0];
    if (c->object_type != -1)
        return 0;
    if (c->num == -10)
        *out_start = true;
    else if (c->num == -11)
        *out_end = true;
    else
        return 0;
    for (int32_t p = 0; p < c->param_count; p++) {
        const fxp_param_t *pa = &c->params[p];
        if (pa->code == FXP_PARAM_GROUP && pa->raw_size >= 8)
            return (int32_t)(pa->raw[6] | (pa->raw[7] << 8));
    }
    return 0;
}

/* Which Fusion group id each row belongs to. */
static int32_t *groups_scan(const fxp_mfa_frame_t *def)
{
    const fxp_events_t *ev = &def->events;
    int32_t *owner = fxp_calloc((size_t)(ev->group_count ? ev->group_count : 1),
                                sizeof(int32_t));
    int32_t stack[64];
    int depth = 0;
    for (int32_t gi = 0; gi < ev->group_count; gi++) {
        bool is_start, is_end;
        int32_t id = group_span_id(&ev->groups[gi], &is_start, &is_end);
        if (is_start) {
            if (depth < 64)
                stack[depth] = id;
            depth++;
            continue;
        }
        if (is_end) {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth > 0)
            owner[gi] = stack[depth - 1];
    }
    return owner;
}

/* Classify every group once (per frame). */
static void groups_classify(fxp_frame_t *frame)
{
    const fxp_events_t *ev = &frame->def->events;
    fxp_runtime_t *rt = frame->rt;
    rt->gclass = fxp_calloc((size_t)(ev->group_count ? ev->group_count : 1), 1);
    rt->gowner = groups_scan(frame->def);
    group_state_ensure(frame, ev->group_count);

    for (int32_t gi = 0; gi < ev->group_count; gi++) {
        const fxp_event_group_t *g = &ev->groups[gi];
        uint8_t cls = FXP_GC_NORMAL;
        if (g->condition_count > 0) {
            const fxp_condition_t *c0 = &g->conditions[0];
            if (c0->object_type == -1 && (c0->num == -10 || c0->num == -11))
                cls = FXP_GC_MARKER;
            else if (c0->object_type == -3 && c0->num == -1)
                cls = FXP_GC_SOF;
            else if (c0->object_type == -3 && c0->num == -2)
                cls = FXP_GC_EOF;
            else if (c0->object_type == -4 &&
                     (c0->num == -8 || c0->num == -4 || c0->num == -7 ||
                      c0->num == -3 || c0->num == -2 || c0->num == -1))
                cls = FXP_GC_TIMER;
            else if (c0->object_type == -1 && c0->num == -9)
                cls = FXP_GC_MARKER;
        }
        for (int32_t c = 0; c < g->condition_count; c++) {
            const fxp_condition_t *cd = &g->conditions[c];
            if (cd->object_type == -4 && cd->num == -6)
                cls = FXP_GC_NAMED;
            else if (cd->object_type == -1 && cd->num == -16)
                cls = FXP_GC_LOOP;
        }
        /* NuclearRT true events (IsTrueEvent): dispatched from
         * DispatchTrueEvents, not run in the normal loop.  Without this
         * classification dispatch_event never fired (FXP_GC_TRUE was
         * never assigned) and these events ran through the normal pass
         * with degenerate checks instead. */
        if (cls == FXP_GC_NORMAL && g->condition_count > 0) {
            const fxp_condition_t *c0 = &g->conditions[0];
            if (c0->object_type == -6 &&
                (c0->num == -9 || c0->num == -5 || c0->num == -6 ||
                 c0->num == -7))
                cls = FXP_GC_TRUE;
            else if (c0->object_type == -7 && c0->num == -4)
                cls = FXP_GC_TRUE;
        }
        rt->gclass[gi] = cls;
    }
}

/* ==================================================== generated events == */

void fxp_runtime_generate_event(fxp_frame_t *frame, int32_t type, int32_t num,
                                fxp_object_t *source)
{
    fxp_runtime_t *rt = frame->rt;
    if (rt->pending_count >= 32)
        return;
    rt->pending[rt->pending_count].type   = (int16_t)type;
    rt->pending[rt->pending_count].num    = (int16_t)num;
    rt->pending[rt->pending_count].source = source;
    rt->pending_count++;
}

/* ======================================================== fast loops ==== */

static fxp_loop_state_t *loop_find(fxp_runtime_t *rt, const char *name)
{
    for (int32_t i = 0; i < rt->loop_count; i++)
        if (loop_name_equals(rt->loops[i].name, name))
            return &rt->loops[i];
    return NULL;
}

static void run_event_pass(fxp_frame_t *frame, int pass);

static void loop_start(fxp_frame_t *frame, const char *name, int32_t count)
{
    fxp_runtime_t *rt = frame->rt;
    char key[128];
    snprintf(key, sizeof(key), "%s", name ? name : "");
    to_lower(key);

    fxp_loop_state_t *l = loop_find(rt, key);
    if (!l) {
        if (rt->loop_count == rt->loop_cap) {
            rt->loop_cap = rt->loop_cap ? rt->loop_cap * 2 : 8;
            rt->loops = fxp_realloc(rt->loops,
                                    (size_t)rt->loop_cap * sizeof(fxp_loop_state_t));
        }
        l = &rt->loops[rt->loop_count++];
        memset(l, 0, sizeof(*l));
        l->name = fxp_strdup(key);
    }
    l->running = true;
    l->index = 0;

    /* StartLoop runs OnLoop inline (Frame.h StartLoop) */
    while (l->running && (l->index < count || count < 0)) {
        rt->current_loop = l->name;
        run_event_pass(frame, FXP_PASS_LOOP);
        rt->current_loop = NULL;
        if (frame->rt->pending_state || !l->running)
            break;
        l->index++;
    }
}

static void loop_stop(fxp_runtime_t *rt, const char *name)
{
    fxp_loop_state_t *l = loop_find(rt, name);
    if (l)
        l->running = false;
}

static int32_t loop_index(fxp_runtime_t *rt, const char *name)
{
    fxp_loop_state_t *l = loop_find(rt, name);
    return l ? l->index : 0;
}

/* ============================================================ timers ==== */

/* GameTimer.CheckEvent (Timer.h): per-event interval state machine. */
static bool timer_check_event(fxp_frame_t *frame, int32_t evt_id,
                              int32_t check_time, int32_t type)
{
    fxp_runtime_t *rt = frame->rt;
    fxp_timer_event_t *evt = NULL;
    for (int32_t i = 0; i < rt->timer_event_count; i++) {
        if (rt->timer_events[i].id == evt_id) {
            evt = &rt->timer_events[i];
            break;
        }
    }
    if (!evt) {
        if (rt->timer_event_count == rt->timer_event_cap) {
            rt->timer_event_cap = rt->timer_event_cap ? rt->timer_event_cap * 2 : 8;
            rt->timer_events = fxp_realloc(
                rt->timer_events,
                (size_t)rt->timer_event_cap * sizeof(fxp_timer_event_t));
        }
        evt = &rt->timer_events[rt->timer_event_count++];
        memset(evt, 0, sizeof(*evt));
        evt->id = evt_id;
    }
    evt->check_time = check_time;
    evt->type = type;

    if (type == 1 && evt->time < (double)check_time)
        evt->triggered = false;           /* Equals re-arms below threshold */
    if (evt->already_checked)
        return false;
    evt->already_checked = true;

    switch (type) {
    case 0:                               /* Every */
        if (check_time <= 0)
            return true;
        if (evt->time >= (double)check_time) {
            evt->time = fmod(evt->time, (double)check_time);
            return true;
        }
        return false;
    case 1:                               /* Equals (fire once) */
        if (!evt->triggered && evt->time >= (double)check_time) {
            evt->triggered = true;
            return true;
        }
        return false;
    case 2:                               /* Less than */
        return evt->time < (double)check_time;
    case 3:                               /* Greater than */
        return evt->time > (double)check_time;
    default:
        return false;
    }
}

static void named_timer_schedule(fxp_frame_t *frame, const char *name,
                                 int32_t count, int32_t initial_delay,
                                 int32_t interval)
{
    fxp_runtime_t *rt = frame->rt;
    if (count <= 0 || !name || !name[0])
        return;
    if (rt->named_timer_count == rt->named_timer_cap) {
        rt->named_timer_cap = rt->named_timer_cap ? rt->named_timer_cap * 2 : 8;
        rt->named_timers = fxp_realloc(
            rt->named_timers,
            (size_t)rt->named_timer_cap * sizeof(fxp_named_timer_t));
    }
    fxp_named_timer_t *t = &rt->named_timers[rt->named_timer_count++];
    memset(t, 0, sizeof(*t));
    t->name = fxp_strdup(name);
    to_lower(t->name);
    t->remaining = count;
    t->interval = interval;
    t->next_fire_time = rt->time + (initial_delay < 0 ? 0 : initial_delay);
    t->index = 0;
}

static void process_named_timers(fxp_frame_t *frame)
{
    fxp_runtime_t *rt = frame->rt;
    for (int32_t i = 0; i < rt->named_timer_count; i++) {
        fxp_named_timer_t *t = &rt->named_timers[i];
        if (rt->time < t->next_fire_time)
            continue;
        free(rt->current_named_event);
        rt->current_named_event = fxp_strdup(t->name);
        run_event_pass(frame, FXP_PASS_NAMED);
        free(rt->current_named_event);
        rt->current_named_event = NULL;
        t->remaining--;
        if (t->remaining > 0)
            t->next_fire_time += t->interval;
        if (t->track_event_index)
            t->index++;
    }
    int32_t w = 0;
    for (int32_t i = 0; i < rt->named_timer_count; i++) {
        if (rt->named_timers[i].remaining > 0)
            rt->named_timers[w++] = rt->named_timers[i];
        else
            free(rt->named_timers[i].name);
    }
    rt->named_timer_count = w;
}

/* ======================================================== selectors ===== */

static fxp_selector_t *selector_by_oi(fxp_frame_t *frame, int32_t oi)
{
    if (oi < 0)
        return NULL;
    return fxp_frame_selector(frame, (uint32_t)oi);
}

/* Frame item list index of an object info (for the -14 collision edge
 * bits); -1 when the OI is a qualifier pseudo-handle. */
static int32_t item_index_of_oi(fxp_frame_t *frame, int32_t oi)
{
    if (!frame->def || oi < 0)
        return -1;
    for (int32_t i = 0; i < frame->def->item_count; i++)
        if (frame->def->items[i]->handle == oi)
            return i;
    return -1;
}

/* ====================================================== expressions ===== */

static fxp_value_t expr_system_call(fxp_frame_t *frame, int32_t num,
                                    fxp_value_t *args, int argc);
static fxp_value_t object_property_value(fxp_frame_t *frame, int32_t type,
                                         int32_t oi_param, int32_t num,
                                         int32_t inline_index,
                                         fxp_object_t *fallback);

/*
 * Fusion expressions are stored INFIX: tokens appear in source order
 * (operand, operator, operand, ...) and must be evaluated with the usual
 * precedence.  Verified against NuclearRT's ExpressionConverter, which
 * re-emits the token stream verbatim as C++ infix code and relies on the
 * C compiler's precedence, and against real Fusion-authored MFAs
 * ("Set X to X(Active) + 2" decodes to [X(Active)][+][2]).
 *
 *   operand  : system literal (num 0/3/23), global (24/50), object
 *              property (type >= 2 or -7), pseudo-object property
 *              (keyboard -6 / create -5 / timer -4 / game -3 / speaker
 *              -2), parenthesised sub-expression, or function call
 *   operator : object_type == 0, num = 2 +, 4 -, 6 *, 8 /, 10 %, 12 pow,
 *              14 & (string concat when either side is a string),
 *              16 |, 18 xor
 *   markers  : system num -1 "(", -2 ")", -3 ","
 */
typedef struct {
    fxp_frame_t            *frame;
    const fxp_expression_t *expr;
    int32_t                 pos;
    int32_t                 depth;
} expr_parser_t;

enum {
    EXPR_PREC_OR    = 1,                  /* |                             */
    EXPR_PREC_XOR   = 2,                  /* xor                           */
    EXPR_PREC_AND   = 3,                  /* & (concat)                    */
    EXPR_PREC_ADD   = 4,                  /* + -                           */
    EXPR_PREC_MUL   = 5,                  /* * / %                         */
    EXPR_PREC_POW   = 6,                  /* power (right associative)     */
};

static int32_t expr_op_prec(int32_t op)
{
    switch (op) {
    case 16: return EXPR_PREC_OR;
    case 18: return EXPR_PREC_XOR;
    case 14: return EXPR_PREC_AND;
    case 2: case 4: return EXPR_PREC_ADD;
    case 6: case 8: case 10: return EXPR_PREC_MUL;
    case 12: return EXPR_PREC_POW;
    default: return 0;                    /* not an operator               */
    }
}

/* Apply a binary operator in place to `a` (ownership of both temps stays
 * with the caller-provided storage). */
static void expr_apply_binop(fxp_value_t *a, fxp_value_t *b, int32_t op)
{
    switch (op) {
    case 2: fxp_value_add(a, b); break;
    case 4: fxp_value_sub(a, b); break;
    case 6: fxp_value_mul(a, b); break;
    case 8: fxp_value_div(a, b); break;
    case 10: fxp_value_mod(a, b); break;
    case 12: {                            /* power                        */
        double r = pow(fxp_value_as_double(a), fxp_value_as_double(b));
        fxp_value_free(a);
        fxp_value_init_double(a, r);
        break;
    }
    case 14: {                            /* & : concat strings           */
        if (a->type == FXP_VAL_STRING || b->type == FXP_VAL_STRING) {
            char la[256], lb[256];
            snprintf(la, sizeof(la), "%s", fxp_value_as_string(a));
            snprintf(lb, sizeof(lb), "%s", fxp_value_as_string(b));
            size_t n = strlen(la) + strlen(lb) + 1;
            char *s = fxp_malloc(n);
            memcpy(s, la, strlen(la));
            memcpy(s + strlen(la), lb, strlen(lb) + 1);
            fxp_value_free(a);
            a->type = FXP_VAL_STRING;
            a->as.s  = s;
            break;
        }
        int32_t r = fxp_value_as_int(a) & fxp_value_as_int(b);
        fxp_value_free(a);
        fxp_value_init_int(a, r);
        break;
    }
    case 16: {                            /* |                            */
        int32_t r = fxp_value_as_int(a) | fxp_value_as_int(b);
        fxp_value_free(a);
        fxp_value_init_int(a, r);
        break;
    }
    case 18: {                            /* xor                          */
        int32_t r = fxp_value_as_int(a) ^ fxp_value_as_int(b);
        fxp_value_free(a);
        fxp_value_init_int(a, r);
        break;
    }
    default:
        break;
    }
    fxp_value_free(b);
}

static const fxp_expr_item_t *expr_peek(expr_parser_t *P)
{
    if (!P->expr || !P->expr->items || P->pos >= P->expr->count)
        return NULL;
    return &P->expr->items[P->pos];
}

static bool expr_at_terminator(const fxp_expr_item_t *it)
{
    return !it || (it->object_type == 0 && it->num == 0);
}

/* True when the token is one of the structural markers: "(" "," ")". */
static bool expr_is_marker(const fxp_expr_item_t *it, int32_t which)
{
    return it && it->object_type == FXP_OT_SYSTEM && it->num == which;
}

static fxp_value_t expr_parse_expr(expr_parser_t *P, int32_t min_prec);

/* Parse one operand (literal / property / parenthesised expr / function
 * call / unary minus). */
static fxp_value_t expr_parse_unary(expr_parser_t *P)
{
    fxp_value_t v;
    fxp_value_init_int(&v, 0);
    if (P->depth > 64) {
        fxp_log_warn("expr: nesting too deep");
        return v;
    }
    P->depth++;

    const fxp_expr_item_t *it = expr_peek(P);
    if (expr_at_terminator(it))
        goto done;                        /* do not consume terminator    */

    if (it->object_type == FXP_OT_SYSTEM) {
        switch (it->num) {
        case -1: {                        /* "("                          */
            P->pos++;
            fxp_value_free(&v);
            v = expr_parse_expr(P, 0);
            const fxp_expr_item_t *close = expr_peek(P);
            if (expr_is_marker(close, -2))
                P->pos++;                 /* ")"                          */
            else if (close && !expr_at_terminator(close) &&
                     !expr_is_marker(close, -3))
                fxp_log_warn("expr: expected ')' at token %d", P->pos);
            goto done;
        }
        case -2: case -3:                 /* ")" "," — not an operand     */
            goto done;
        case 0:
            P->pos++;
            fxp_value_free(&v);
            fxp_value_init_int(&v, it->int_val);
            goto done;
        case 3:
            P->pos++;
            fxp_value_free(&v);
            fxp_value_init_string(&v, it->str_val ? it->str_val : "");
            goto done;
        case 23:
            P->pos++;
            fxp_value_free(&v);
            fxp_value_init_double(&v, it->dbl_val);
            goto done;
        case 24: case 50: {               /* global value / string        */
            P->pos++;
            fxp_value_t *arr = it->num == 24 ? P->frame->rt->global_values
                                             : P->frame->rt->global_strings;
            int32_t n = it->num == 24 ? P->frame->rt->global_value_count
                                      : P->frame->rt->global_string_count;
            int32_t idx = it->int_val;
            fxp_value_free(&v);
            fxp_value_init_int(&v, 0);
            if (idx >= 0 && idx < n)
                fxp_value_copy(&v, &arr[idx]);
            else if (it->num == 50)
                fxp_value_init_string(&v, "");
            goto done;
        }
        default: {                        /* function call fn(args...)    */
            int32_t fn = it->num;
            P->pos++;
            fxp_value_t args[8];
            int32_t argc = 0;
            const fxp_expr_item_t *open = expr_peek(P);
            if (expr_is_marker(open, -1)) {
                P->pos++;                 /* consume "("                  */
                const fxp_expr_item_t *nx = expr_peek(P);
                if (expr_is_marker(nx, -2)) {
                    P->pos++;             /* "()"                         */
                } else {
                    for (;;) {
                        if (argc < 8) {
                            args[argc] = expr_parse_expr(P, 0);
                            argc++;
                        } else {
                            fxp_value_t sink = expr_parse_expr(P, 0);
                            fxp_value_free(&sink);
                        }
                        const fxp_expr_item_t *sep = expr_peek(P);
                        if (expr_is_marker(sep, -3)) {
                            P->pos++;     /* ","                          */
                            continue;
                        }
                        const fxp_expr_item_t *close = expr_peek(P);
                        if (expr_is_marker(close, -2))
                            P->pos++;     /* ")"                          */
                        break;
                    }
                }
            } else if (open && !expr_at_terminator(open) &&
                       !expr_is_marker(open, -3) && !expr_is_marker(open, -2)) {
                /* defensive: function token without a "(" — treat the
                 * next operand as its single argument */
                args[0] = expr_parse_unary(P);
                argc = 1;
            }
            fxp_value_free(&v);
            v = expr_system_call(P->frame, fn, args, argc);
            for (int32_t k = 0; k < argc; k++)
                fxp_value_free(&args[k]);
            goto done;
        }
        }
    }

    if (it->object_type == 0) {           /* operator in unary position   */
        if (it->num == 4) {               /* unary minus                  */
            P->pos++;
            fxp_value_free(&v);
            v = expr_parse_unary(P);
            if (v.type == FXP_VAL_DOUBLE)
                v.as.d = -v.as.d;
            else if (v.type == FXP_VAL_INT)
                v.as.i = -v.as.i;
            else {
                int32_t r = -fxp_value_as_int(&v);
                fxp_value_free(&v);
                fxp_value_init_int(&v, r);
            }
            goto done;
        }
        if (it->num == 2) {               /* unary plus                   */
            P->pos++;
            fxp_value_free(&v);
            v = expr_parse_unary(P);
            goto done;
        }
        fxp_log_warn("expr: operator %d in operand position", it->num);
        P->pos++;
        goto done;
    }

    /* object / player / pseudo-object property */
    {
        int32_t type = it->object_type;
        int32_t num = it->num;
        int32_t inline_index = -1;
        P->pos++;
        if (type >= 2 || type == FXP_OT_PLAYER) {
            int32_t oi = (int32_t)it->object_info;
            if (num == 30 || num == 31 || num == 13) {
                /* index is a parenthesised sub-expression, or (legacy
                 * XPort fixture form) the next system LONG token        */
                const fxp_expr_item_t *nx = expr_peek(P);
                if (expr_is_marker(nx, -1)) {
                    P->pos++;
                    fxp_value_t idxv = expr_parse_expr(P, 0);
                    inline_index = fxp_value_as_int(&idxv);
                    fxp_value_free(&idxv);
                    const fxp_expr_item_t *close = expr_peek(P);
                    if (expr_is_marker(close, -2))
                        P->pos++;
                } else if (nx && nx->object_type == FXP_OT_SYSTEM &&
                           nx->num == 0) {
                    inline_index = nx->int_val;
                    P->pos++;
                }
            } else if (num == 16 || num == 19) {
                inline_index = it->ext_val;
            }
            fxp_value_free(&v);
            v = object_property_value(P->frame, type, oi, num,
                                      inline_index, NULL);
            goto done;
        }
        /* keyboard / create / timer / game / speaker property */
        fxp_value_free(&v);
        v = object_property_value(P->frame, type, 0, num, -1, NULL);
    }

done:
    P->depth--;
    return v;
}

/* Precedence-climbing infix evaluation (operator tokens: object_type 0). */
static fxp_value_t expr_parse_expr(expr_parser_t *P, int32_t min_prec)
{
    fxp_value_t lhs = expr_parse_unary(P);
    for (;;) {
        const fxp_expr_item_t *it = expr_peek(P);
        if (expr_at_terminator(it))
            break;
        if (it->object_type != 0)
            break;                        /* not an operator              */
        int32_t prec = expr_op_prec(it->num);
        if (prec == 0 || prec < min_prec)
            break;
        int32_t op = it->num;
        P->pos++;
        fxp_value_t rhs = (op == 12)
            ? expr_parse_expr(P, prec)    /* right associative            */
            : expr_parse_expr(P, prec + 1);
        expr_apply_binop(&lhs, &rhs, op);
    }
    return lhs;
}

/* Evaluate an infix expression stream (ExpressionConverter token order). */
fxp_value_t fxp_expr_eval(fxp_frame_t *frame, const fxp_expression_t *expr,
                          fxp_object_t *instance, uint32_t oi)
{
    fxp_runtime_t *rt = frame->rt;
    fxp_value_t out;
    fxp_value_init_int(&out, 0);
    if (!expr || !expr->items)
        return out;

    fxp_object_t *saved_inst = rt->loop_instance;
    uint32_t saved_oi = rt->loop_oi;
    bool saved_have = rt->have_loop_instance;
    if (instance) {
        rt->loop_instance = instance;
        rt->loop_oi = oi;
        rt->have_loop_instance = true;
    }

    expr_parser_t P = { frame, expr, 0, 0 };
    out = expr_parse_expr(&P, 0);

    /* Free any tokens left unconsumed by malformed input (literals may
     * own strings — the items themselves are freed by the owner, but a
     * partially consumed stream must not double-count anything: values
     * built during parsing are all returned or freed internally).        */
    rt->loop_instance = saved_inst;
    rt->loop_oi = saved_oi;
    rt->have_loop_instance = saved_have;
    return out;
}

/* System function implementations (ExpressionConverter lookup). */
static fxp_value_t expr_system_call(fxp_frame_t *frame, int32_t num,
                                    fxp_value_t *args, int argc)
{
    fxp_runtime_t *rt = frame->rt;
    fxp_value_t r;
    fxp_value_init_int(&r, 0);
    char buf[512];
    double a0 = argc > 0 ? fxp_value_as_double(&args[0]) : 0.0;
    int32_t i0 = argc > 0 ? fxp_value_as_int(&args[0]) : 0;

    switch (num) {
    case 1:                               /* Random(n) */
        fxp_value_init_int(&r, rt_random(rt, i0));
        break;
    case 2: {                             /* Global value by expr (1-based) */
        int32_t idx = i0 - 1;
        if (idx >= 0 && idx < rt->global_value_count)
            fxp_value_copy(&r, &rt->global_values[idx]);
        break;
    }
    case 4:                               /* Val(str) */
        fxp_value_init_double(&r, atof(fxp_value_as_string(&args[0])));
        break;
    case 5:                               /* Str$(n) */
        snprintf(buf, sizeof(buf), "%g", a0);
        fxp_value_init_string(&r, buf);
        break;
    case 10: fxp_value_init_double(&r, sin(a0 * 3.14159265358979 / 180.0)); break;
    case 11: fxp_value_init_double(&r, cos(a0 * 3.14159265358979 / 180.0)); break;
    case 12: fxp_value_init_double(&r, tan(a0 * 3.14159265358979 / 180.0)); break;
    case 13: fxp_value_init_double(&r, sqrt(a0)); break;
    case 14: fxp_value_init_double(&r, a0 > 0 ? log10(a0) : 0); break;
    case 15: fxp_value_init_double(&r, a0 > 0 ? log(a0) : 0); break;
    case 16: snprintf(buf, sizeof(buf), "%X", i0);
             fxp_value_init_string(&r, buf); break;               /* Hex$ */
    case 18: fxp_value_init_double(&r, exp(a0)); break;
    case 19: {                            /* Left$(s, n) */
        const char *s = fxp_value_as_string(&args[0]);
        int32_t n = argc > 1 ? fxp_value_as_int(&args[1]) : 0;
        int32_t len = (int32_t)strlen(s);
        if (n > len) n = len;
        if (n < 0) n = 0;
        if (n > 511) n = 511;
        memcpy(buf, s, (size_t)n); buf[n] = 0;
        fxp_value_init_string(&r, buf);
        break;
    }
    case 20: {                            /* Right$(s, n) */
        const char *s = fxp_value_as_string(&args[0]);
        int32_t n = argc > 1 ? fxp_value_as_int(&args[1]) : 0;
        int32_t len = (int32_t)strlen(s);
        if (n > len) n = len;
        if (n < 0) n = 0;
        fxp_value_init_string(&r, s + len - n);
        break;
    }
    case 21: {                            /* Mid$(s, pos, n) */
        const char *s = fxp_value_as_string(&args[0]);
        int32_t pos = argc > 1 ? fxp_value_as_int(&args[1]) : 1;
        int32_t n = argc > 2 ? fxp_value_as_int(&args[2]) : 0x7FFFFFF;
        int32_t len = (int32_t)strlen(s);
        if (pos < 1) pos = 1;
        if (pos > len + 1) pos = len + 1;
        if (n < 0) n = 0;
        if (n > 511) n = 511;
        memcpy(buf, s + pos - 1, (size_t)n); buf[n] = 0;
        fxp_value_init_string(&r, buf);
        break;
    }
    case 22:                              /* Len(s) */
        fxp_value_init_int(&r, (int32_t)strlen(fxp_value_as_string(&args[0])));
        break;
    case 28: fxp_value_init_int(&r, (int32_t)trunc(a0)); break;   /* Int */
    case 29: fxp_value_init_double(&r, fabs(a0)); break;          /* Abs */
    case 30: fxp_value_init_double(&r, ceil(a0)); break;          /* Ceil */
    case 31: fxp_value_init_double(&r, floor(a0)); break;         /* Floor */
    case 32: fxp_value_init_double(&r,
                                   acos(a0 < -1 ? -1 : (a0 > 1 ? 1 : a0)) *
                                   180.0 / 3.14159265358979); break;
    case 33: fxp_value_init_double(&r,
                                   asin(a0 < -1 ? -1 : (a0 > 1 ? 1 : a0)) *
                                   180.0 / 3.14159265358979); break;
    case 34: fxp_value_init_double(&r,
                                   atan(a0) * 180.0 / 3.14159265358979); break;
    case 35: fxp_value_init_int(&r, ~i0); break;                  /* NOT */
    case 40: {                            /* Min */
        double a1 = argc > 1 ? fxp_value_as_double(&args[1]) : a0;
        fxp_value_init_double(&r, a0 < a1 ? a0 : a1);
        break;
    }
    case 41: {                            /* Max */
        double a1 = argc > 1 ? fxp_value_as_double(&args[1]) : a0;
        fxp_value_init_double(&r, a0 > a1 ? a0 : a1);
        break;
    }
    case 42: {                            /* GetRGB(r,g,b) -> 0x00bbggrr */
        int32_t gg = argc > 1 ? fxp_value_as_int(&args[1]) : 0;
        int32_t bb = argc > 2 ? fxp_value_as_int(&args[2]) : 0;
        fxp_value_init_int(&r, (i0 & 0xFF) | ((gg & 0xFF) << 8) |
                               ((bb & 0xFF) << 16));
        break;
    }
    case 43: fxp_value_init_int(&r, i0 & 0xFF); break;            /* GetRed */
    case 44: fxp_value_init_int(&r, (i0 >> 8) & 0xFF); break;     /* GetGreen */
    case 45: fxp_value_init_int(&r, (i0 >> 16) & 0xFF); break;    /* GetBlue */
    case 46: {                            /* LoopIndex(name) */
        const char *s = fxp_value_as_string(&args[0]);
        fxp_value_init_int(&r, rt->current_loop && loop_name_equals(
                                   rt->current_loop, s)
                                   ? loop_index(rt, s) : 0);
        break;
    }
    case 47: fxp_value_init_string(&r, "\n"); break;              /* NewLine$ */
    case 48: fxp_value_init_int(&r, (int32_t)llround(a0)); break; /* Round */
    case 49: {                            /* Global string by expr (1-based) */
        int32_t idx = i0 - 1;
        if (idx >= 0 && idx < rt->global_string_count)
            fxp_value_copy(&r, &rt->global_strings[idx]);
        else
            fxp_value_init_string(&r, "");
        break;
    }
    case 51: {                            /* Lower$(s) */
        snprintf(buf, sizeof(buf), "%s", fxp_value_as_string(&args[0]));
        to_lower(buf);
        fxp_value_init_string(&r, buf);
        break;
    }
    case 52: {                            /* Upper$(s) */
        snprintf(buf, sizeof(buf), "%s", fxp_value_as_string(&args[0]));
        for (char *p = buf; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        fxp_value_init_string(&r, buf);
        break;
    }
    case 53: {                            /* Find(hay, needle) */
        const char *h = fxp_value_as_string(&args[0]);
        const char *n = argc > 1 ? fxp_value_as_string(&args[1]) : "";
        const char *p = strstr(h, n);
        fxp_value_init_int(&r, p ? (int32_t)(p - h) + 1 : 0);
        break;
    }
    case 54: {                            /* ReverseFind(hay, needle) */
        const char *h = fxp_value_as_string(&args[0]);
        const char *n = argc > 1 ? fxp_value_as_string(&args[1]) : "";
        const char *p = strrchr(h, n[0]);
        fxp_value_init_int(&r, p ? (int32_t)(p - h) + 1 : 0);
        break;
    }
    case 58: fxp_value_copy(&r, &args[0]); break; /* (cast) */
    case 59: {                            /* ATan2(y, x) */
        double a1 = argc > 1 ? fxp_value_as_double(&args[1]) : 0.0;
        fxp_value_init_double(&r, atan2(a0, a1) * 180.0 / 3.14159265358979);
        break;
    }
    case 62: {                            /* Distance(dx, dy) */
        double a1 = argc > 1 ? fxp_value_as_double(&args[1]) : 0.0;
        fxp_value_init_double(&r, sqrt(a0 * a0 + a1 * a1));
        break;
    }
    case 64: {                            /* Range(v, min, max) */
        double lo = argc > 1 ? fxp_value_as_double(&args[1]) : 0.0;
        double hi = argc > 2 ? fxp_value_as_double(&args[2]) : 0.0;
        fxp_value_init_double(&r, a0 < lo ? lo : (a0 > hi ? hi : a0));
        break;
    }
    case 65: {                            /* RRandom(min, max) */
        int32_t a1 = argc > 1 ? fxp_value_as_int(&args[1]) : i0;
        int32_t lo = i0 < a1 ? i0 : a1;
        int32_t hi = i0 < a1 ? a1 : i0;
        fxp_value_init_int(&r, lo + rt_random(rt, hi - lo + 1));
        break;
    }
    case 67:                              /* RuntimeName$ */
        fxp_value_init_string(&r, "Fusion X-Port");
        break;
    default:
        fxp_log_warn("expr: unimplemented system function %d", num);
        break;
    }
    return r;
}

/* Image lookup by handle (for geometry). */
static const fxp_image_t *image_by_handle(const fxp_frame_t *frame,
                                          int32_t handle)
{
    if (!frame->mfa || handle < 0)
        return NULL;
    for (int32_t i = 0; i < frame->mfa->images.count; i++)
        if (frame->mfa->images.items[i].handle == handle)
            return &frame->mfa->images.items[i];
    return NULL;
}

static int32_t object_current_image_handle(const fxp_object_t *obj)
{
    return fxp_object_current_image(obj);
}

int32_t fxp_object_image_width(const fxp_frame_t *frame, const fxp_object_t *obj)
{
    if (obj->type == FXP_OT_QUICK_BACKDROP)
        return obj->item->as.quick_backdrop.w;
    int32_t img = obj->type == FXP_OT_BACKDROP
                      ? (int32_t)obj->item->as.backdrop.image
                      : object_current_image_handle(obj);
    const fxp_image_t *im = image_by_handle(frame, img);
    return im ? im->width : 0;
}

int32_t fxp_object_image_height(const fxp_frame_t *frame, const fxp_object_t *obj)
{
    if (obj->type == FXP_OT_QUICK_BACKDROP)
        return obj->item->as.quick_backdrop.h;
    int32_t img = obj->type == FXP_OT_BACKDROP
                      ? (int32_t)obj->item->as.backdrop.image
                      : object_current_image_handle(obj);
    const fxp_image_t *im = image_by_handle(frame, img);
    return im ? im->height : 0;
}

/* Instance context for object property reads: the iterated instance when
 * the expression references its OI, else the first selected instance. */
static fxp_object_t *property_instance(fxp_frame_t *frame, int32_t oi)
{
    fxp_runtime_t *rt = frame->rt;
    if (rt->have_loop_instance && (int32_t)rt->loop_oi == oi)
        return rt->loop_instance;
    fxp_selector_t *sel = selector_by_oi(frame, oi);
    return sel ? fxp_selector_first(sel) : NULL;
}

static int32_t count_all_of_oi(fxp_frame_t *frame, int32_t oi)
{
    fxp_selector_t *sel = selector_by_oi(frame, oi);
    return sel ? sel->all_count : 0;
}

static int32_t count_selected_of_oi(fxp_frame_t *frame, int32_t oi)
{
    fxp_selector_t *sel = selector_by_oi(frame, oi);
    return sel ? fxp_selector_count(sel) : 0;
}

/* Object/system property read (ExpressionConverter object cases). */
static fxp_value_t object_property_value(fxp_frame_t *frame, int32_t type,
                                         int32_t oi_param, int32_t num,
                                         int32_t inline_index,
                                         fxp_object_t *fallback)
{
    fxp_runtime_t *rt = frame->rt;
    fxp_value_t v;
    fxp_value_init_int(&v, 0);

    switch (type) {
    case -7: {                            /* player */
        int32_t p = oi_param;
        if (p < 0 || p > 3) p = 0;
        if (num == 0)
            fxp_value_init_int(&v, rt->score[p]);
        else if (num == 1)
            fxp_value_init_int(&v, rt->lives[p]);
        break;
    }
    case -6:                              /* keyboard/mouse */
        if (num == 0) fxp_value_init_int(&v, rt->input.mouse_x);
        else if (num == 1) fxp_value_init_int(&v, rt->input.mouse_y);
        else if (num == 2) fxp_value_init_int(&v, 0);
        break;
    case -5:                              /* create */
        if (num == 0)
            fxp_value_init_int(&v, frame->instance_count);
        break;
    case -4:                              /* timer */
        switch (num) {
        case 0: fxp_value_init_int(&v, (int32_t)rt->time); break;
        case 1: fxp_value_init_int(&v, ((int32_t)rt->time / 10) % 100); break;
        case 2: fxp_value_init_int(&v, (int32_t)(rt->time / 1000) % 60); break;
        case 3: fxp_value_init_int(&v, (int32_t)(rt->time / 3600000)); break;
        case 4: fxp_value_init_int(&v, (int32_t)(rt->time / 60000) % 60); break;
        default: break;
        }
        break;
    case -3:                              /* game */
        switch (num) {
        case 0: case 8:
            fxp_value_init_int(&v, frame->def->handle + 1);
            break;
        case 6: case 11:
            fxp_value_init_int(&v, frame->def->size_x);
            break;
        case 7: case 12:
            fxp_value_init_int(&v, frame->def->size_y);
            break;
        case 2: fxp_value_init_int(&v, (int32_t)frame->scroll_x); break;
        case 3: fxp_value_init_int(&v,
                                   (int32_t)(frame->scroll_x + FXP_SCREEN_W));
            break;
        case 4: fxp_value_init_int(&v, (int32_t)frame->scroll_y); break;
        case 5: fxp_value_init_int(&v,
                                   (int32_t)(frame->scroll_y + FXP_SCREEN_H));
            break;
        case 10: fxp_value_init_int(&v, 60); break;
        case 13: {
            fxp_color_t bg = frame->def->background;
            fxp_value_init_int(&v, bg.r | (bg.g << 8) | (bg.b << 16));
            break;
        }
        default: break;
        }
        break;
    case -2: case -1:                     /* speaker / none */
        break;
    default: {                            /* real object */
        int32_t oi = oi_param;            /* callers pass (type, objinfo) */
        fxp_object_t *obj = fallback ? fallback : property_instance(frame, oi);
        if (num == 15) {                  /* NObjects */
            fxp_value_init_int(&v, count_all_of_oi(frame, oi));
            break;
        }
        if (num == 45) {                  /* NSelectedObjects */
            fxp_value_init_int(&v, count_selected_of_oi(frame, oi));
            break;
        }
        if (!obj)
            break;

        switch (num) {
        case 1:  fxp_value_init_int(&v, obj->y); break;           /* Y */
        case 11: fxp_value_init_int(&v, obj->x); break;           /* X */
        case 12:                                                  /* Fixed */
            fxp_value_init_int(&v,
                               (int32_t)((obj->handle << 16) |
                                         ((obj->handle - 1) & 0xFFFF)));
            break;
        case 2: {                                                 /* Image */
            fxp_value_init_int(&v, object_current_image_handle(obj));
            break;
        }
        case 3: fxp_value_init_int(&v, (int32_t)obj->speed); break;
        case 6: fxp_value_init_int(&v, obj->direction); break;    /* Dir */
        case 14: fxp_value_init_int(&v, obj->anim_frame); break;  /* Anim frame */
        case 18: fxp_value_init_int(&v, obj->anim_seq); break;    /* NMovement */
        case 23: fxp_value_init_int(&v, (int32_t)obj->layer); break;
        case 27: fxp_value_init_int(&v, obj->ink_effect_param); break;
        case 28: fxp_value_init_int(&v, obj->rgb_coefficient); break;
        case 29: fxp_value_init_int(&v, obj->ink_effect_param); break;
        case 16: {                        /* AltVal (inline index) */
            fxp_value_t *av = fxp_object_value(obj,
                                               inline_index < 0 ? 0 : inline_index);
            if (av) fxp_value_copy(&v, av);
            break;
        }
        case 19: {                        /* AltStr (inline index) */
            const char *s = fxp_object_string(obj,
                                              inline_index < 0 ? 0 : inline_index);
            fxp_value_init_string(&v, s);
            break;
        }
        case 30: {                        /* AltValN (index operand, 1-based) */
            fxp_value_t *av = fxp_object_value(obj, inline_index - 1);
            if (av) fxp_value_copy(&v, av);
            break;
        }
        case 31: {                        /* AltStrN$ (index operand) */
            const char *s = fxp_object_string(obj, inline_index - 1);
            fxp_value_init_string(&v, s);
            break;
        }
        case 13: {                        /* Flag(index): value != 0 */
            fxp_value_t *av = fxp_object_value(obj, inline_index - 1);
            fxp_value_init_int(&v, (av && fxp_value_as_int(av)) ? 1 : 0);
            break;
        }
        case 40: fxp_value_init_int(&v, fxp_object_image_width(frame, obj)); break;
        case 41: fxp_value_init_int(&v, fxp_object_image_height(frame, obj)); break;
        case 25: case 26: {                                       /* Action pt */
            int32_t img = object_current_image_handle(obj);
            const fxp_image_t *im = image_by_handle(frame, img);
            fxp_value_init_int(&v, num == 25 ? (im ? im->action_x : 0)
                                             : (im ? im->action_y : 0));
            break;
        }
        case 83: fxp_value_init_int(&v, (int32_t)obj->angle); break;
        case 81: case 82: fxp_value_init_int(&v, 100); break;     /* scale */
        default:
            break;
        }
        break;
    }
    }
    return v;
}

/* ============================================== parameter accessors ===== */

/* Parameter payload starts after the [size i16][code i16] header. */
static const uint8_t *param_payload(const fxp_param_t *p)
{
    return p->raw ? p->raw + 4 : NULL;
}

/* `offset` is relative to the payload (raw already skips the
 * [size i16][code i16] header). */
static int32_t param_i16(const fxp_param_t *p, int32_t offset)
{
    const uint8_t *d = param_payload(p);
    if (!d || p->raw_size < offset + 6)
        return 0;
    return (int32_t)(int16_t)(d[offset] | (d[offset + 1] << 8));
}

static int32_t param_u16(const fxp_param_t *p, int32_t offset)
{
    const uint8_t *d = param_payload(p);
    if (!d || p->raw_size < offset + 6)
        return 0;
    return (int32_t)(d[offset] | (d[offset + 1] << 8));
}

static int32_t param_i32(const fxp_param_t *p, int32_t offset)
{
    const uint8_t *d = param_payload(p);
    if (!d || p->raw_size < offset + 8)
        return 0;
    return (int32_t)((uint32_t)d[offset] |
                     ((uint32_t)d[offset + 1] << 8) |
                     ((uint32_t)d[offset + 2] << 16) |
                     ((uint32_t)d[offset + 3] << 24));
}

/*
 * Yuuniversal string embedded in a raw parameter payload (sample-file /
 * music-file params: i16 handle, u16 flags, u16 len, UTF-16LE chars).
 * Returns a malloc'd UTF-8 copy (caller frees) or NULL.
 */
static char *param_embedded_name(const fxp_param_t *p)
{
    const uint8_t *d = param_payload(p);
    if (!d || p->raw_size < 8)
        return NULL;
    int32_t len = (int32_t)(d[4] | (d[5] << 8));
    if (len < 0 || (size_t)6 + (size_t)len * 2 > (size_t)p->raw_size - 4)
        return NULL;
    size_t cap = (size_t)len * 4 + 1;
    char *out = fxp_malloc(cap);
    size_t o = 0;
    for (int32_t i = 0; i < len; i++) {
        uint32_t ch = (uint32_t)(d[6 + i * 2] | (d[6 + i * 2 + 1] << 8));
        if (ch < 0x80) {
            if (o + 2 > cap) break;
            out[o++] = (char)ch;
        } else if (ch < 0x800) {
            if (o + 3 > cap) break;
            out[o++] = (char)(0xC0 | (ch >> 6));
            out[o++] = (char)(0x80 | (ch & 0x3F));
        } else {
            if (o + 4 > cap) break;
            out[o++] = (char)(0xE0 | (ch >> 12));
            out[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

/* Value of a "number-ish" parameter (Short/Int/Global/Alterable/Key/Time). */
static fxp_value_t param_value(const fxp_param_t *p)
{
    fxp_value_t v;
    fxp_value_init_int(&v, 0);
    if (!p)
        return v;
    if (p->has_expression)
        return v;                         /* callers use param_expr_eval */
    /* Value of a "number-ish" parameter (Short/Int/Global/Alterable/Key/Time). */
    switch (p->code) {
    case FXP_PARAM_SHORT:
    case 4: case 10: case 11: case 12: case 17: case 26: case 31:
    case 43: case 57: case 58: case 60: case 61:
    case FXP_PARAM_ALTERABLE:         /* 50: i16 = 0-based value index */
        fxp_value_init_int(&v, param_i16(p, 0));
        break;
    case FXP_PARAM_INT:
    case 25: case 29: case 34: case 48: case 56: case 67: case 70:
    case FXP_PARAM_GLOBALVALUE:
        fxp_value_init_int(&v, param_i32(p, 0));
        break;
    case FXP_PARAM_EVERY:                 /* Every: i32 delay, i32 compteur */
    case 2:                               /* Time: i32 timer, i32 loops */
        fxp_value_init_int(&v, param_i32(p, 0));
        break;
    case FXP_PARAM_KEY:                   /* Key: u16 (codes 14 AND 44) */
    case 44:
        fxp_value_init_int(&v, param_u16(p, 0));
        break;
    default:
        break;
    }
    return v;
}

/* Evaluate an expression-or-constant parameter. */
static fxp_value_t param_expr_eval(fxp_frame_t *frame, const fxp_param_t *p,
                                   fxp_object_t *instance, uint32_t oi)
{
    if (p && p->has_expression)
        return fxp_expr_eval(frame, &p->expression, instance, oi);
    return param_value(p);
}

static const fxp_param_t *param_of(const fxp_condition_t *c, int32_t idx)
{
    return c && idx >= 0 && idx < c->param_count ? &c->params[idx] : NULL;
}
static const fxp_param_t *aparam_of(const fxp_action_t *a, int32_t idx)
{
    return a && idx >= 0 && idx < a->param_count ? &a->params[idx] : NULL;
}

/* Comparison code carried by an expression parameter. */
static int32_t param_comparison(const fxp_param_t *p)
{
    if (p && p->has_expression)
        return p->expression.comparison;
    return 0;
}

/* ================================================== relevant objectinfos */

static void collect_ois_from_params(fxp_frame_t *frame, const fxp_param_t *p,
                                    int32_t *ois, int32_t *count, int32_t max);

static void collect_oi(fxp_frame_t *frame, int32_t oi, int32_t *ois,
                       int32_t *count, int32_t max)
{
    (void)frame;
    /* NOTE: object info handles are 0-based in CTFAK-decompiled MFAs
     * (CCN handles start at 0), so oi == 0 is a valid object.  Skipping
     * it meant its selector was never reset -> stale selections leaked
     * between events ("events target the wrong object"). */
    if (oi < 0 || *count >= max)
        return;
    for (int32_t i = 0; i < *count; i++)
        if (ois[i] == oi)
            return;
    ois[(*count)++] = oi;
}

static void collect_ois_from_expr(fxp_frame_t *frame, const fxp_expression_t *e,
                                  int32_t *ois, int32_t *count, int32_t max)
{
    if (!e || !e->items)
        return;
    for (int32_t i = 0; i < e->count; i++) {
        const fxp_expr_item_t *it = &e->items[i];
        if (it->object_type > 0)
            collect_oi(frame, (int32_t)it->object_info, ois, count, max);
    }
}

static void collect_ois_from_params(fxp_frame_t *frame, const fxp_param_t *p,
                                    int32_t *ois, int32_t *count, int32_t max)
{
    if (!p || !p->raw)
        return;
    if (p->has_expression) {
        collect_ois_from_expr(frame, &p->expression, ois, count, max);
        return;
    }
    switch (p->code) {
    case FXP_PARAM_OBJECT: {              /* ParamObject: list,oi,type */
        collect_oi(frame, param_u16(p, 2), ois, count, max);
        break;
    }
    case FXP_PARAM_POSITION: {            /* Position */
        int32_t parent = param_u16(p, 0);
        if (parent != 0xFFFF)
            collect_oi(frame, parent, ois, count, max);
        break;
    }
    case FXP_PARAM_CREATE: {              /* Create: Position(24) + oi @26 */
        int32_t parent = param_u16(p, 0);
        if (parent != 0xFFFF)
            collect_oi(frame, parent, ois, count, max);
        collect_oi(frame, param_u16(p, 26), ois, count, max);
        break;
    }
    case FXP_PARAM_SHOOT: {               /* Shoot: Position + instance/oi */
        int32_t parent = param_u16(p, 0);
        if (parent != 0xFFFF)
            collect_oi(frame, parent, ois, count, max);
        collect_oi(frame, param_u16(p, 26), ois, count, max);
        break;
    }
    default:
        break;
    }
}

static int32_t collect_event_ois(fxp_frame_t *frame, int16_t object_type,
                                 uint16_t object_info,
                                 fxp_param_t *params, int32_t param_count,
                                 int32_t *ois, int32_t max)
{
    int32_t count = 0;
    if (object_type > 0)
        collect_oi(frame, (int32_t)object_info, ois, &count, max);
    for (int32_t i = 0; i < param_count; i++)
        collect_ois_from_params(frame, &params[i], ois, &count, max);
    return count;
}

static int32_t collect_cond_ois(fxp_frame_t *frame, const fxp_condition_t *c,
                                int32_t *ois, int32_t max)
{
    return collect_event_ois(frame, c->object_type, c->object_info,
                             c->params, c->param_count, ois, max);
}

static int32_t collect_action_ois(fxp_frame_t *frame, const fxp_action_t *a,
                                  int32_t *ois, int32_t max)
{
    return collect_event_ois(frame, a->object_type, a->object_info,
                             a->params, a->param_count, ois, max);
}

/* ==================================================== geometry helpers == */

static bool aabb_overlap(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                         int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Bounding box of an instance (image at position, no rotation). */
static void object_bbox(fxp_frame_t *frame, const fxp_object_t *obj,
                        int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    *w = fxp_object_image_width(frame, obj);
    *h = fxp_object_image_height(frame, obj);
    *x = obj->x;
    *y = obj->y;
}

/*
 * Fine (pixel-perfect) detection — NuclearRT rule: Active objects use
 * pixel-perfect collisions unless NewFlags has the CollisionBox bit
 * (0x0004); backdrop images always use their mask; everything else
 * (quick backdrops, text, counters, scores) is box-only.
 */
static bool object_is_fine(const fxp_object_t *obj)
{
    if (obj->type == FXP_OT_BACKDROP)
        return true;
    if (obj->type != FXP_OT_ACTIVE)
        return false;
    return (obj->item->common.new_object_flags & 0x0004) == 0;
}

/* 1bpp mask bit test (NuclearRT IsPixelSolid layout). */
static bool mask_solid(const fxp_image_t *im, int32_t lx, int32_t ly)
{
    if (!im || !im->col_mask || lx < 0 || ly < 0 ||
        lx >= im->width || ly >= im->height)
        return false;
    return (im->col_mask[(size_t)ly * im->col_mask_stride + (size_t)(lx >> 3)]
            & (0x80 >> (lx & 7))) != 0;
}

/*
 * Instance collision (NuclearRT Frame::IsColliding): AABB pre-test, then
 * a per-pixel scan of the intersection.  Objects in fine-detection mode
 * sample their image's alpha mask; box-mode objects are solid across
 * their whole bounding box (so box-vs-mask tests only sample the mask,
 * and box-vs-box reduces to the AABB result).
 */
static bool instances_collide(fxp_frame_t *frame, const fxp_object_t *a,
                              const fxp_object_t *b)
{
    int32_t ax, ay, aw, ah, bx, by, bw, bh;
    object_bbox(frame, a, &ax, &ay, &aw, &ah);
    object_bbox(frame, b, &bx, &by, &bw, &bh);

    int32_t x0 = ax > bx ? ax : bx;
    int32_t y0 = ay > by ? ay : by;
    int32_t x1 = (ax + aw) < (bx + bw) ? (ax + aw) : (bx + bw);
    int32_t y1 = (ay + ah) < (by + bh) ? (ay + ah) : (by + bh);
    if (x0 >= x1 || y0 >= y1)
        return false;

    const fxp_image_t *ia = object_is_fine(a)
        ? image_by_handle(frame, object_current_image_handle(a)) : NULL;
    const fxp_image_t *ib = object_is_fine(b)
        ? image_by_handle(frame, object_current_image_handle(b)) : NULL;
    bool mask_a = ia && ia->col_mask;
    bool mask_b = ib && ib->col_mask;
    if (!mask_a && !mask_b)
        return true;                  /* box x box — AABB already hit  */

    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            if (mask_a && !mask_solid(ia, x - ax, y - ay))
                continue;
            if (mask_b && !mask_solid(ib, x - bx, y - by))
                continue;
            return true;
        }
    }
    return false;
}

/* Public hook for tests/tools. */
bool fxp_collision_objects(fxp_frame_t *frame, fxp_object_t *a, fxp_object_t *b)
{
    return instances_collide(frame, a, b);
}

/* Backdrop collision: any backdrop instance (pixel-perfect against the
 * object's own collision mode) / quick-backdrop fill (box). */
static bool collides_with_background(fxp_frame_t *frame, const fxp_object_t *obj)
{
    int32_t ax, ay, aw, ah;
    object_bbox(frame, obj, &ax, &ay, &aw, &ah);
    for (int32_t i = 0; i < frame->instance_count; i++) {
        const fxp_object_t *b = frame->instances[i];
        if (b == obj || b->marked_for_delete)
            continue;
        if (b->type == FXP_OT_BACKDROP) {
            if (instances_collide(frame, obj, b))
                return true;
            continue;
        }
        if (b->type == FXP_OT_QUICK_BACKDROP) {
            int32_t bw = b->item->as.quick_backdrop.w;
            int32_t bh = b->item->as.quick_backdrop.h;
            if (aabb_overlap(ax, ay, aw, ah, b->x, b->y, bw, bh))
                return true;
        }
    }
    return false;
}

static bool mouse_over_instance(fxp_frame_t *frame, const fxp_object_t *obj)
{
    fxp_runtime_t *rt = frame->rt;
    int32_t x, y, w, h;
    object_bbox(frame, obj, &x, &y, &w, &h);
    return rt->input.mouse_x >= x && rt->input.mouse_x < x + w &&
           rt->input.mouse_y >= y && rt->input.mouse_y < y + h;
}

/* ================================================== condition dispatch == */

/* Key down / pressed by Windows VK code (Input.h IsKeysDown/Pressed). */
static bool vk_down(fxp_frame_t *frame, int32_t vk)
{
    if (vk < 0 || vk > 255)
        return false;
    return frame->rt->input.keys_down[vk] != 0;
}

static bool vk_pressed(fxp_frame_t *frame, int32_t vk)
{
    if (vk < 0 || vk > 255)
        return false;
    return frame->rt->input.keys_pressed[vk] != 0;
}

static int32_t controls_of_player(int32_t player)
{
    return player >= 0 && player < 4 ? player : 0;
}

/* NOTE: control conditions use BITMASK semantics now — the Short
 * parameter is a mask (bit N = control N), applied directly against
 * input.controls_down/pressed in eval_condition. */

static bool any_control_pressed(fxp_frame_t *frame)
{
    fxp_runtime_t *rt = frame->rt;
    for (int32_t p = 0; p < 4; p++)
        if (rt->input.controls_pressed[p])
            return true;
    return false;
}

/* Evaluate one condition (no negation — the caller flips).  Filter
 * conditions deselect non-matching instances and return whether any
 * instance remains (ObjectIterator + deselect semantics). */
static bool eval_condition(fxp_frame_t *frame, int32_t gi,
                           const fxp_condition_t *c,
                           fxp_object_t *true_source)
{
    fxp_runtime_t *rt = frame->rt;
    (void)true_source;
    int32_t type = c->object_type;
    int32_t num = c->num;
    int32_t oi = (int32_t)c->object_info;

    /* -------- system conditions -------- */
    if (type == -1) {
        switch (num) {
        case -1:                          /* Always */
            return true;
        case -2:                          /* Never */
            return false;
        case -7:                          /* One-action-loop marker */
            return true;
        case -16: {                       /* On loop */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            bool match = rt->current_loop &&
                         loop_name_equals(rt->current_loop,
                                          fxp_value_as_string(&v));
            fxp_value_free(&v);
            return match;
        }
        case -25:                         /* OR marker (segment logic) */
            return true;
        case -26: {                       /* Chance */
            fxp_value_t a = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            fxp_value_t b = param_expr_eval(frame, param_of(c, 1), NULL, 0);
            int32_t chance = fxp_value_as_int(&a);
            int32_t max = fxp_value_as_int(&b);
            fxp_value_free(&a);
            fxp_value_free(&b);
            if (chance >= max)
                return true;
            return rt_random(rt, max) < chance;
        }
        case -3: {                        /* Compare general values */
            fxp_value_t a = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            fxp_value_t b = param_expr_eval(frame, param_of(c, 1), NULL, 0);
            bool r = fxp_compare(param_comparison(param_of(c, 1)),
                                 fxp_value_cmp(&a, &b));
            fxp_value_free(&a);
            fxp_value_free(&b);
            return r;
        }
        case -8: {                        /* Compare global value */
            const fxp_param_t *p0 = param_of(c, 0);
            fxp_value_t gv;
            fxp_value_init_int(&gv, 0);
            if (p0 && !p0->has_expression &&
                p0->code == FXP_PARAM_GLOBALVALUE) {
                int32_t idx = param_i32(p0, 0);
                if (idx >= 0 && idx < rt->global_value_count)
                    fxp_value_copy(&gv, &rt->global_values[idx]);
            } else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                int32_t idx = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
                if (idx >= 0 && idx < rt->global_value_count)
                    fxp_value_copy(&gv, &rt->global_values[idx]);
            }
            fxp_value_t b = param_expr_eval(frame, param_of(c, 1), NULL, 0);
            bool r = fxp_compare(param_comparison(param_of(c, 1)),
                                 fxp_value_cmp(&gv, &b));
            fxp_value_free(&gv);
            fxp_value_free(&b);
            return r;
        }
        case -20: {                       /* Compare global strings */
            const fxp_param_t *p0 = param_of(c, 0);
            char buf_g[256] = "";
            if (p0 && !p0->has_expression &&
                p0->code == FXP_PARAM_GLOBALVALUE) {
                int32_t idx = param_i32(p0, 0);
                if (idx >= 0 && idx < rt->global_string_count)
                    snprintf(buf_g, sizeof(buf_g), "%s",
                             fxp_value_as_string(&rt->global_strings[idx]));
            }
            fxp_value_t b = param_expr_eval(frame, param_of(c, 1), NULL, 0);
            int cmp = strcmp(buf_g, fxp_value_as_string(&b));
            fxp_value_free(&b);
            return fxp_compare(param_comparison(param_of(c, 1)), cmp);
        }
        case -6:                          /* Run once */
            group_state_ensure(frame, frame->def->events.group_count);
            if (rt->run_once[gi])
                return false;
            rt->run_once[gi] = true;
            return true;
        case -10: case -11:               /* group markers — never here */
            return true;
        default:
            warn_once(type, num, "system condition");
            return true;
        }
    }

    /* -------- game conditions -------- */
    if (type == -3) {
        if (num == -1)                    /* Start of frame */
            return rt->update_count <= 2;
        if (num == -2)                    /* End of frame */
            return frame->at_eof;
        warn_once(type, num, "game condition");
        return true;
    }

    /* -------- timer conditions -------- */
    if (type == -4) {
        const fxp_param_t *p0 = param_of(c, 0);
        int32_t ms = 0;
        if (p0 && !p0->has_expression &&
            (p0->code == FXP_PARAM_EVERY || p0->code == 2))
            ms = param_i32(p0, 0);
        else {
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            ms = fxp_value_as_int(&v);
            fxp_value_free(&v);
        }
        int32_t t;
        switch (num) {
        case -8: case -4: t = 0; break;   /* Every */
        case -7: case -3: t = 1; break;   /* Equals */
        case -2: t = 2; break;            /* Less than */
        case -1: t = 3; break;            /* Greater than */
        case -6:                          /* Named timer event */
            return rt->current_named_event != NULL;
        default:
            warn_once(type, num, "timer condition");
            return true;
        }
        return timer_check_event(frame, gi, ms, t);
    }

    /* -------- keyboard / mouse conditions -------- */
    if (type == -6) {
        switch (num) {
        case -9:                          /* Upon pressing any key */
            return rt->input.any_key_pressed;
        case -1: {                        /* Upon pressing a key */
            fxp_value_t v = param_value(param_of(c, 0));
            bool r = vk_pressed(frame, fxp_value_as_int(&v));
            fxp_value_free(&v);
            return r;
        }
        case -2: {                        /* Repeat while key down */
            fxp_value_t v = param_value(param_of(c, 0));
            bool r = vk_down(frame, fxp_value_as_int(&v));
            fxp_value_free(&v);
            return r;
        }
        case -5:                          /* Upon clicking */
            return rt->input.mouse_clicked;
        case -6: {                        /* Clicking in zone */
            if (!rt->input.mouse_clicked)
                return false;
            const fxp_param_t *zone = NULL;
            for (int32_t p = 0; p < c->param_count; p++)
                if (c->params[p].code == FXP_PARAM_ZONE)
                    zone = &c->params[p];
            if (!zone)
                return true;
            int32_t x1 = param_i16(zone, 0), y1 = param_i16(zone, 2);
            int32_t x2 = param_i16(zone, 4), y2 = param_i16(zone, 6);
            if (x2 < x1) { int32_t t = x1; x1 = x2; x2 = t; }
            if (y2 < y1) { int32_t t = y1; y1 = y2; y2 = t; }
            return rt->input.mouse_x >= x1 && rt->input.mouse_x < x2 &&
                   rt->input.mouse_y >= y1 && rt->input.mouse_y < y2;
        }
        case -3: {                        /* Mouse over zone (held) */
            const fxp_param_t *zone = NULL;
            for (int32_t p = 0; p < c->param_count; p++)
                if (c->params[p].code == FXP_PARAM_ZONE)
                    zone = &c->params[p];
            if (!zone)
                return true;
            int32_t x1 = param_i16(zone, 0), y1 = param_i16(zone, 2);
            int32_t x2 = param_i16(zone, 4), y2 = param_i16(zone, 6);
            if (x2 < x1) { int32_t t = x1; x1 = x2; x2 = t; }
            if (y2 < y1) { int32_t t = y1; y1 = y2; y2 = t; }
            return rt->input.mouse_x >= x1 && rt->input.mouse_x < x2 &&
                   rt->input.mouse_y >= y1 && rt->input.mouse_y < y2;
        }
        case -8:                          /* Repeat while mouse pressed */
            return rt->input.mouse_down;
        default:
            warn_once(type, num, "keyboard condition");
            return true;
        }
    }

    /* -------- player control conditions --------
     * The Short parameter is a CONTROL BITMASK (bit N = control N; NuclearRT
     * IsControlsDown/Pressed does `control & (1 << i)`).  Treating it as an
     * index shifted every mapping one slot (Up checked Down, Left checked
     * Button1, ...) — the "input fails to register" bug. */
    if (type == -7) {
        int32_t p = controls_of_player(oi);
        switch (num) {
        case -4: case -1: {               /* Control(s) pressed (true event) */
            fxp_value_t v = param_value(param_of(c, 0));
            int32_t mask = fxp_value_as_int(&v);
            fxp_value_free(&v);
            return (rt->input.controls_pressed[p] & mask) != 0;
        }
        case -6: {                        /* Control(s) down */
            fxp_value_t v = param_value(param_of(c, 0));
            int32_t mask = fxp_value_as_int(&v);
            fxp_value_free(&v);
            /* NuclearRT IsControlsDown: EVERY masked control must be held */
            return (rt->input.controls_down[p] & mask) == mask;
        }
        default:
            warn_once(type, num, "player condition");
            return true;
        }
    }

    /* -------- object conditions (filter semantics) -------- */
    if (type >= 0) {
        fxp_selector_t *sel = selector_by_oi(frame, oi);
        if (!sel) {
            fxp_log_warn("events: condition on unknown object info %d", oi);
            return false;
        }

        switch (num) {
        /* position comparisons (keep instances satisfying the compare) */
        case -17:                         /* compare X */
        case -16: {                       /* compare Y */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t cmp_code = param_comparison(param_of(c, 0));
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                rt->loop_instance = obj;
                rt->loop_oi = (uint32_t)oi;
                rt->have_loop_instance = true;
                fxp_value_t pv = param_expr_eval(frame, param_of(c, 0), obj,
                                                 (uint32_t)oi);
                int32_t pos = num == -17 ? obj->x : obj->y;
                bool keep = fxp_compare(cmp_code, pos < fxp_value_as_int(&pv)
                                                     ? -1
                                                     : (pos > fxp_value_as_int(&pv)
                                                            ? 1 : 0));
                fxp_value_free(&pv);
                if (!keep)
                    fxp_selector_iter_deselect_current(sel);
            }
            rt->have_loop_instance = false;
            fxp_value_free(&v);
            return fxp_selector_count(sel) > 0;
        }

        case -27: {                       /* Compare alterable value */
            const fxp_param_t *p0 = param_of(c, 0);
            const fxp_param_t *p1 = param_of(c, 1);
            /* Same index encoding as the add/subtract/spread actions:
             * the non-expression ALTERABLE param (code 50, and Short
             * variants) carries the 0-based value index DIRECTLY.  The
             * old code only accepted Short params here, so real Fusion
             * files (code 50) evaluated index -1 and the comparison
             * never passed — "collect 5 gems" never triggered the win
             * frame.  Only the expression form is 1-based (NuclearRT
             * AlterableValueComparisonCondition). */
            int32_t index;
            if (p0 && !p0->has_expression) {
                index = param_i16(p0, 0);
            } else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                index = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
            }
            int32_t cmp_code = param_comparison(p1);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t pv = param_expr_eval(frame, p1, obj, (uint32_t)oi);
                fxp_value_t *av = fxp_object_value(obj, index);
                int cmp = av ? fxp_value_cmp(av, &pv) : -1;
                fxp_value_free(&pv);
                if (!fxp_compare(cmp_code, cmp))
                    fxp_selector_iter_deselect_current(sel);
            }
            return fxp_selector_count(sel) > 0;
        }

        case -25:                         /* Flag is on */
        case -24: {                       /* Flag is off */
            const fxp_param_t *p0 = param_of(c, 0);
            fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
            int32_t index = fxp_value_as_int(&e) - 1;   /* 1-based flags */
            fxp_value_free(&e);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t *av = fxp_object_value(obj, index);
                bool on = av && fxp_value_as_int(av) != 0;
                if (on != (num == -25))
                    fxp_selector_iter_deselect_current(sel);
            }
            return fxp_selector_count(sel) > 0;
        }

        case -1: {                        /* Compare animation frame */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t cmp_code = param_comparison(param_of(c, 0));
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int cmp = obj->anim_frame < fxp_value_as_int(&v)
                              ? -1
                              : (obj->anim_frame > fxp_value_as_int(&v) ? 1 : 0);
                if (!fxp_compare(cmp_code, cmp))
                    fxp_selector_iter_deselect_current(sel);
            }
            fxp_value_free(&v);
            return fxp_selector_count(sel) > 0;
        }

        case -2: {                        /* Animation over */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (!obj->anim_over)
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -3: {                        /* Animation playing */
            fxp_value_t v = param_value(param_of(c, 0));
            int32_t want = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (obj->anim_seq != want)
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -8: {                        /* Compare direction */
            fxp_value_t v = param_value(param_of(c, 0));
            int32_t want = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (obj->direction != want)
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -15: {                       /* Compare movement speed */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t cmp_code = param_comparison(param_of(c, 0));
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int cmp = obj->speed < fxp_value_as_double(&v)
                              ? -1
                              : (obj->speed > fxp_value_as_double(&v) ? 1 : 0);
                if (!fxp_compare(cmp_code, cmp))
                    fxp_selector_iter_deselect_current(sel);
            }
            fxp_value_free(&v);
            return fxp_selector_count(sel) > 0;
        }

        case -4: {                        /* Overlapping another object */
            const fxp_param_t *p0 = param_of(c, 0);
            int32_t other_oi = p0 ? param_u16(p0, 2) : -1;
            fxp_selector_t *other = selector_by_oi(frame, other_oi);
            if (!other)
                return c->other_flags != 0;
            bool any = false;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                bool collides = false;
                fxp_selector_iter_begin(other);
                fxp_object_t *o;
                while ((o = fxp_selector_iter_next(other)) != NULL) {
                    if (o != obj && instances_collide(frame, obj, o)) {
                        collides = true;
                        break;
                    }
                }
                if (c->other_flags & 1) {   /* negated: pure boolean */
                    if (collides) {
                        any = true;
                        break;
                    }
                } else if (!collides) {
                    /* positive: keep only the colliding instances
                     * (nuclearrt deselects non-colliders) */
                    fxp_selector_iter_deselect_current(sel);
                } else {
                    any = true;
                }
            }
            if (c->other_flags & 1)
                return !any;
            /* positive: narrow the other selector to colliders too */
            fxp_selector_iter_begin(other);
            fxp_object_t *o;
            while ((o = fxp_selector_iter_next(other)) != NULL) {
                bool collides = false;
                fxp_selector_iter_begin(sel);
                fxp_object_t *o2;
                while ((o2 = fxp_selector_iter_next(sel)) != NULL) {
                    if (o2 != o && instances_collide(frame, o, o2)) {
                        collides = true;
                        break;
                    }
                }
                if (!collides)
                    fxp_selector_iter_deselect_current(other);
            }
            return fxp_selector_count(sel) > 0;
        }

        case -14: {                       /* Is colliding with an object
                                           * (edge-triggered: fires when a
                                           * NEW collision begins — the
                                           * gem-collection idiom).  Falls
                                           * back to continuous overlap for
                                           * frame item indices >= 64.     */
            const fxp_param_t *p0 = param_of(c, 0);
            int32_t other_oi = p0 ? param_u16(p0, 2) : -1;
            fxp_selector_t *other = selector_by_oi(frame, other_oi);
            if (!other)
                return c->other_flags != 0;
            int32_t oidx = item_index_of_oi(frame, other_oi);
            uint64_t bit = (oidx >= 0 && oidx < 64) ? (1ull << oidx) : 0;
            bool any = false;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                bool collides = false;
                fxp_selector_iter_begin(other);
                fxp_object_t *o;
                while ((o = fxp_selector_iter_next(other)) != NULL) {
                    if (o != obj && instances_collide(frame, obj, o)) {
                        collides = true;
                        break;
                    }
                }
                bool fresh = collides && !(bit && (obj->collide_seen & bit));
                if (bit) {
                    if (collides)
                        obj->collide_seen |= bit;
                    else
                        obj->collide_seen &= ~bit;
                }
                if (c->other_flags & 1) {   /* negated: pure boolean      */
                    if (fresh) {
                        any = true;
                        break;
                    }
                } else if (!fresh) {
                    fxp_selector_iter_deselect_current(sel);
                } else {
                    any = true;
                }
            }
            if (c->other_flags & 1)
                return !any;
            /* positive: narrow the other selector to instances involved
             * in the fresh collisions */
            fxp_selector_iter_begin(other);
            fxp_object_t *o;
            while ((o = fxp_selector_iter_next(other)) != NULL) {
                bool collides = false;
                fxp_selector_iter_begin(sel);
                fxp_object_t *o2;
                while ((o2 = fxp_selector_iter_next(sel)) != NULL) {
                    if (o2 != o && instances_collide(frame, o, o2)) {
                        collides = true;
                        break;
                    }
                }
                if (!collides)
                    fxp_selector_iter_deselect_current(other);
            }
            return fxp_selector_count(sel) > 0;
        }

        case -23: {                       /* Overlapping a backdrop */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (!collides_with_background(frame, obj))
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -32: {                       /* Compare object count */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t cmp_code = param_comparison(param_of(c, 0));
            int32_t n = sel->all_count;
            int cmp = n < fxp_value_as_int(&v)
                          ? -1
                          : (n > fxp_value_as_int(&v) ? 1 : 0);
            fxp_value_free(&v);
            return fxp_compare(cmp_code, cmp);
        }

        case -34: {                       /* Pick one of */
            if (fxp_selector_count(sel) == 0)
                return false;
            int32_t pick = rt_random(rt, fxp_selector_count(sel));
            fxp_selector_select_only(sel, sel->selected[pick]);
            return true;
        }

        case -47: {                       /* Pick lowest/highest expr */
            fxp_value_t best;
            fxp_value_init_int(&best, 0);
            fxp_object_t *best_obj = NULL;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, param_of(c, 0), obj,
                                                (uint32_t)oi);
                if (!best_obj ||
                    fxp_value_cmp(&v, &best) < 0) {
                    fxp_value_free(&best);
                    best = v;
                    best_obj = obj;
                } else {
                    fxp_value_free(&v);
                }
            }
            if (best_obj)
                fxp_selector_select_only(sel, best_obj);
            fxp_value_free(&best);
            return fxp_selector_count(sel) > 0;
        }

        case -46: {                       /* Current layer comparison */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t cmp_code = param_comparison(param_of(c, 0));
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int32_t l = (int32_t)obj->layer;
                int cmp = l < fxp_value_as_int(&v)
                              ? -1
                              : (l > fxp_value_as_int(&v) ? 1 : 0);
                if (!fxp_compare(cmp_code, cmp))
                    fxp_selector_iter_deselect_current(sel);
            }
            fxp_value_free(&v);
            return fxp_selector_count(sel) > 0;
        }

        case -28:                         /* Is invisible */
        case -29: {                       /* Is visible */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (obj->visible != (num == -29))
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -45: {                       /* Object compare expression */
            const fxp_param_t *p0 = param_of(c, 0);
            const fxp_param_t *p1 = param_of(c, 1);
            fxp_value_t b = param_expr_eval(frame, p1, NULL, 0);
            int32_t cmp_code = param_comparison(p1);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t a = param_expr_eval(frame, p0, obj, (uint32_t)oi);
                int cmp = fxp_value_cmp(&a, &b);
                fxp_value_free(&a);
                if (!fxp_compare(cmp_code, cmp))
                    fxp_selector_iter_deselect_current(sel);
            }
            fxp_value_free(&b);
            return fxp_selector_count(sel) > 0;
        }

        case -7: {                        /* Movement stopped */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                if (obj->speed > 0.0)
                    fxp_selector_iter_deselect_current(sel);
            return fxp_selector_count(sel) > 0;
        }

        case -22: {                       /* Close to window border */
            fxp_value_t v = param_expr_eval(frame, param_of(c, 0), NULL, 0);
            int32_t dist = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                bool close =
                    obj->x <= (int32_t)frame->scroll_x + dist ||
                    obj->y <= (int32_t)frame->scroll_y + dist ||
                    obj->x >= (int32_t)(frame->scroll_x + FXP_SCREEN_W - dist) ||
                    obj->y >= (int32_t)(frame->scroll_y + FXP_SCREEN_H - dist);
                if (!close)
                    fxp_selector_iter_deselect_current(sel);
            }
            return fxp_selector_count(sel) > 0;
        }

        default:
            warn_once(type, num, "object condition");
            return true;
        }
    }

    warn_once(type, num, "condition");
    return true;
}

/* Mouse-click-on-object condition helper: filter selection to instances
 * under the mouse. */
static bool filter_under_mouse(fxp_frame_t *frame, int32_t oi)
{
    fxp_selector_t *sel = selector_by_oi(frame, oi);
    if (!sel)
        return false;
    fxp_selector_iter_begin(sel);
    fxp_object_t *obj;
    while ((obj = fxp_selector_iter_next(sel)) != NULL)
        if (!mouse_over_instance(frame, obj))
            fxp_selector_iter_deselect_current(sel);
    return fxp_selector_count(sel) > 0;
}

/* ===================================================== action dispatch == */

static void exec_action(fxp_frame_t *frame, fxp_action_t *a)
{
    fxp_runtime_t *rt = frame->rt;
    int32_t type = a->object_type;
    int32_t num = a->num;
    int32_t oi = (int32_t)a->object_info;

    /* -------- global/system actions -------- */
    if (type == -1) {
        switch (num) {
        case 3: case 4: case 5: {         /* set/sub/add global value */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t val = param_expr_eval(frame, aparam_of(a, 1), NULL, 0);
            int32_t idx = -1;
            if (p0 && !p0->has_expression &&
                p0->code == FXP_PARAM_GLOBALVALUE) {
                idx = param_i32(p0, 0);
            } else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                idx = fxp_value_as_int(&e) - 1;   /* expression form 1-based */
                fxp_value_free(&e);
            }
            if (idx >= 0) {
                if (idx >= rt->global_value_count) {
                    int32_t old = rt->global_value_count;
                    rt->global_values = fxp_realloc(
                        rt->global_values, (size_t)(idx + 1) * sizeof(fxp_value_t));
                    for (int32_t k = old; k <= idx; k++)
                        fxp_value_init_int(&rt->global_values[k], 0);
                    rt->global_value_count = idx + 1;
                }
                fxp_value_t *gv = &rt->global_values[idx];
                if (num == 3)
                    fxp_value_copy(gv, &val);
                else if (num == 4)
                    fxp_value_sub(gv, &val);
                else
                    fxp_value_add(gv, &val);
            }
            fxp_value_free(&val);
            break;
        }
        case 19: {                        /* set global string */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t val = param_expr_eval(frame, aparam_of(a, 1), NULL, 0);
            int32_t idx = 0;
            if (p0 && !p0->has_expression &&
                p0->code == FXP_PARAM_GLOBALVALUE) {
                idx = param_i32(p0, 0);
            } else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                idx = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
            }
            if (idx >= 0) {
                if (idx >= rt->global_string_count) {
                    int32_t old = rt->global_string_count;
                    rt->global_strings = fxp_realloc(
                        rt->global_strings, (size_t)(idx + 1) * sizeof(fxp_value_t));
                    for (int32_t k = old; k <= idx; k++)
                        fxp_value_init_string(&rt->global_strings[k], "");
                    rt->global_string_count = idx + 1;
                }
                fxp_value_free(&rt->global_strings[idx]);
                fxp_value_init_string(&rt->global_strings[idx],
                                      fxp_value_as_string(&val));
            }
            fxp_value_free(&val);
            break;
        }
        case 14: {                        /* Start loop */
            fxp_value_t name = param_expr_eval(frame, aparam_of(a, 0), NULL, 0);
            fxp_value_t count = param_expr_eval(frame, aparam_of(a, 1), NULL, 0);
            loop_start(frame, fxp_value_as_string(&name),
                       fxp_value_as_int(&count));
            fxp_value_free(&name);
            fxp_value_free(&count);
            break;
        }
        case 15: {                        /* Stop loop */
            fxp_value_t name = param_expr_eval(frame, aparam_of(a, 0), NULL, 0);
            loop_stop(rt, fxp_value_as_string(&name));
            fxp_value_free(&name);
            break;
        }
        case 42: {                        /* Send to output */
            fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), NULL, 0);
            fxp_log_info("output: %s", fxp_value_as_string(&v));
            fxp_value_free(&v);
            break;
        }
        case 6: case 7: {                 /* activate / deactivate group */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t id = 0;
            if (p0 && p0->code == FXP_PARAM_GROUP && p0->raw_size >= 8)
                id = (int32_t)(p0->raw[6] | (p0->raw[7] << 8));
            if (id > 0) {
                group_ids_ensure(frame, id);
                rt->group_active[id] = (num == 6);
            }
            break;
        }
        case 0:                           /* empty */
            break;
        default:
            warn_once(type, num, "system action");
            break;
        }
        return;
    }

    /* -------- application actions -------- */
    if (type == -3) {
        switch (num) {
        case 0: rt->pending_state = 1; break;                   /* next frame */
        case 1: rt->pending_state = 2; break;                   /* prev frame */
        case 2: {                                               /* jump frame */
            /* Fusion's plain frame param (code 26) is the target frame's
             * HANDLE as stored in the MFA frame list — NOT a position.
             * Verified against real build-295 files: eventest's frames
             * carry handles [0,1] (param 1 = win frame) while other
             * files use 1-based handles [1,2,...]; treating the value
             * as a position landed jumps one scene off ("wrong scene").
             * The app resolves the handle to a position at load time
             * (with a position fallback for 0-based files).
             * Expression variant: the editor stores the human frame
             * NUMBER (1-based) -> NuclearRT subtracts 1 -> position. */
            const fxp_param_t *p0 = aparam_of(a, 0);
            if (p0 && p0->has_expression) {
                fxp_value_t v = fxp_expr_eval(frame, &p0->expression,
                                              NULL, 0);
                rt->pending_frame = fxp_value_as_int(&v) - 1;
                fxp_value_free(&v);
                rt->pending_frame_is_handle = false;
            } else {
                fxp_value_t v = param_value(p0);
                rt->pending_frame = fxp_value_as_int(&v);
                fxp_value_free(&v);
                rt->pending_frame_is_handle = true;
            }
            rt->pending_state = 3;
            break;
        }
        case 4: rt->pending_state = 5; break;                   /* end app */
        case 23: {                                              /* bg color */
            const fxp_param_t *p0 = aparam_of(a, 0);
            if (p0 && p0->raw && p0->raw_size >= 8) {
                frame->background_override.r = p0->raw[4 + 1];
                frame->background_override.g = p0->raw[4 + 2];
                frame->background_override.b = p0->raw[4 + 3];
                frame->background_override.a = 255;
                frame->has_background_override = true;
            }
            break;
        }
        case 7: {                       /* Center display at position   */
            /* NuclearRT CenterDisplayAtAction: a Position param —
             * absolute (parent 0xFFFF) or relative to every selected
             * instance of the parent object (last one wins). */
            const fxp_param_t *p0 = aparam_of(a, 0);
            if (p0 && p0->code == FXP_PARAM_POSITION) {
                int32_t parent = param_u16(p0, 0);
                int32_t px = param_i16(p0, 4);
                int32_t py = param_i16(p0, 6);
                if (parent == 0xFFFF) {
                    fxp_frame_set_scroll(frame, (double)px, (double)py, -1);
                } else {
                    fxp_selector_t *psel = selector_by_oi(frame, parent);
                    if (psel) {
                        fxp_selector_iter_begin(psel);
                        fxp_object_t *parent_obj;
                        while ((parent_obj =
                                fxp_selector_iter_next(psel)) != NULL) {
                            fxp_frame_set_scroll(frame,
                                                 (double)(parent_obj->x + px),
                                                 (double)(parent_obj->y + py),
                                                 (int32_t)parent_obj->layer);
                        }
                    }
                }
            }
            break;
        }
        case 8: {                       /* Center display at X          */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            fxp_frame_set_scroll_x(frame, (double)fxp_value_as_int(&v));
            fxp_value_free(&v);
            break;
        }
        case 9: {                       /* Center display at Y          */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            fxp_frame_set_scroll_y(frame, (double)fxp_value_as_int(&v));
            fxp_value_free(&v);
            break;
        }
        case 14: case 15:               /* fullscreen / windowed        */
            break;
        default:
            warn_once(type, num, "app action");
            break;
        }
        return;
    }

    /* -------- timer actions -------- */
    if (type == -4) {
        switch (num) {
        case 0: {                         /* Set timer */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            rt->time = (double)fxp_value_as_int(&v);
            fxp_value_free(&v);
            break;
        }
        case 1: {                         /* Fire event (once after delay) */
            const fxp_param_t *p0 = aparam_of(a, 0);
            const fxp_param_t *p1 = aparam_of(a, 1);
            int32_t delay = 0;
            if (p0 && !p0->has_expression && p0->code == 2)
                delay = param_i32(p0, 0);
            else {
                fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
                delay = fxp_value_as_int(&v);
                fxp_value_free(&v);
            }
            fxp_value_t name = param_expr_eval(frame, p1, NULL, 0);
            named_timer_schedule(frame, fxp_value_as_string(&name), 1, delay, 0);
            fxp_value_free(&name);
            break;
        }
        case 2: {                         /* Fire event N times */
            const fxp_param_t *p0 = aparam_of(a, 0);
            const fxp_param_t *p1 = aparam_of(a, 1);
            const fxp_param_t *p2 = aparam_of(a, 2);
            const fxp_param_t *p3 = aparam_of(a, 3);
            int32_t delay = 0, count = 1, interval = 0;
            if (p0 && !p0->has_expression && p0->code == 2)
                delay = param_i32(p0, 0);
            else {
                fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
                delay = fxp_value_as_int(&v);
                fxp_value_free(&v);
            }
            fxp_value_t c = param_expr_eval(frame, p1, NULL, 0);
            count = fxp_value_as_int(&c);
            fxp_value_free(&c);
            if (p2 && !p2->has_expression && p2->code == 2)
                interval = param_i32(p2, 0);
            else {
                fxp_value_t v = param_expr_eval(frame, p2, NULL, 0);
                interval = fxp_value_as_int(&v);
                fxp_value_free(&v);
            }
            fxp_value_t name = param_expr_eval(frame, p3, NULL, 0);
            named_timer_schedule(frame, fxp_value_as_string(&name), count,
                                 delay, interval);
            fxp_value_free(&name);
            break;
        }
        default:
            warn_once(type, num, "timer action");
            break;
        }
        return;
    }

    /* -------- create actions -------- */
    if (type == -5) {
        const fxp_param_t *p0 = aparam_of(a, 0);
        if (num == 2 && p0 && p0->code == FXP_PARAM_OBJECT) {
            /* Create at: ParamObject + x, y, layer expressions */
            int32_t new_oi = param_u16(p0, 2);
            fxp_item_t *item = fxp_mfa_frame_item(frame->def, new_oi);
            if (item) {
                fxp_value_t xv = param_expr_eval(frame, aparam_of(a, 1), NULL, 0);
                fxp_value_t yv = param_expr_eval(frame, aparam_of(a, 2), NULL, 0);
                fxp_value_t lv = param_expr_eval(frame, aparam_of(a, 3), NULL, 0);
                int32_t layer = fxp_value_as_int(&lv) - 1;
                if (layer < 0)
                    layer = 0;
                fxp_object_t *obj = fxp_frame_create_instance(
                    frame, item, fxp_value_as_int(&xv), fxp_value_as_int(&yv),
                    layer);
                fxp_value_free(&xv);
                fxp_value_free(&yv);
                fxp_value_free(&lv);
                if (obj) {
                    fxp_selector_t *sel = selector_by_oi(frame, new_oi);
                    if (sel) {
                        fxp_selector_add(sel, obj);
                        fxp_selector_select_only(sel, obj);
                    }
                }
            }
        } else if (num == 0 && p0 && p0->code == FXP_PARAM_CREATE) {
            /* Create (by position param): Position(24) + count + oi */
            int32_t new_oi = param_u16(p0, 26);
            fxp_item_t *item = fxp_mfa_frame_item(frame->def, new_oi);
            if (item) {
                int32_t x = param_i16(p0, 4);
                int32_t y = param_i16(p0, 6);
                int32_t parent = param_u16(p0, 0);
                if (parent != 0xFFFF) {
                    fxp_object_t *parent_obj = property_instance(frame, parent);
                    if (parent_obj) {
                        x += parent_obj->x;
                        y += parent_obj->y;
                    }
                }
                fxp_object_t *obj = fxp_frame_create_instance(frame, item, x, y, 0);
                if (obj) {
                    fxp_selector_t *sel = selector_by_oi(frame, new_oi);
                    if (sel) {
                        fxp_selector_add(sel, obj);
                        fxp_selector_select_only(sel, obj);
                    }
                }
            }
        }
        return;
    }

    /* -------- score / lives (player) -------- */
    if (type == -7) {
        int32_t p = controls_of_player(oi);
        const fxp_param_t *p0 = aparam_of(a, 0);
        fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
        int32_t val = fxp_value_as_int(&v);
        fxp_value_free(&v);
        switch (num) {
        case 0: rt->score[p] = val; break;
        case 4: rt->score[p] += val; break;
        case 6: rt->score[p] -= val; break;
        case 1: rt->lives[p] = val; break;
        case 5: rt->lives[p] += val; break;
        case 7: rt->lives[p] -= val; break;
        case 2: case 3: case 8: case 9:  /* control ignore/restore/set key */
            break;
        default:
            warn_once(type, num, "player action");
            break;
        }
        return;
    }

    /* -------- keyboard (mouse cursor visibility is a desktop nicety) --- */
    if (type == -6) {
        return;                           /* hide/show cursor: no-op on 3DS */
    }

    /* -------- sample & music actions -------- */
    if (type == -2) {
        const fxp_param_t *p0 = aparam_of(a, 0);
        if (!p0)
            return;
        /* Param family: 6 = sample handle, 7 = sample by name,
         * 35 = music handle, 36 = music by name (CTFAK LoadParameter
         * maps all four to its Sample class).  Samples and music live
         * in different banks but play through the same mixer. */
        bool is_music = (p0->code == 35 || p0->code == 36);
        int32_t handle = -1;
        char *name_tmp = NULL;
        const char *by_name = NULL;
        if (!rt->audio || !rt->mfa || !rt->audio_load)
            return;
        if (p0->code == 6 || p0->code == 35) {
            handle = param_i16(p0, 0);
        } else {
            by_name = name_tmp = param_embedded_name(p0);
            if (!by_name)
                return;                   /* unusable param */
        }

        /* bank lookup: exact handle match first, then the raw position
         * (some writers store a shifted handle space), then by name. */
        int32_t slot = -1;
        if (is_music) {
            const fxp_music_t *items = rt->mfa->music_items;
            int32_t count = rt->mfa->music_count;
            for (int32_t i = 0; i < count; i++) {
                if (by_name ? (items[i].name && strcmp(items[i].name,
                                                        by_name) == 0)
                            : (items[i].handle == handle)) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0 && !by_name && handle >= 0 && handle < count)
                slot = handle;
            if (slot >= 0 && rt->sample_channel[64 + slot] < 0 &&
                items[slot].data) {
                rt->sample_channel[64 + slot] = rt->audio_load(
                    rt->audio, items[slot].data,
                    (size_t)items[slot].data_size);
            }
        } else {
            const fxp_sound_t *items = rt->mfa->sounds.items;
            int32_t count = rt->mfa->sounds.count;
            for (int32_t i = 0; i < count; i++) {
                if (by_name ? (items[i].name && strcmp(items[i].name,
                                                        by_name) == 0)
                            : (items[i].handle == handle)) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0 && !by_name && handle >= 0 && handle < count)
                slot = handle;
            if (slot >= 0 && rt->sample_channel[slot] < 0 &&
                items[slot].data) {
                rt->sample_channel[slot] = rt->audio_load(
                    rt->audio, items[slot].data,
                    (size_t)items[slot].data_size);
            }
        }

        /* the play/stop calls target the mixer channel that audio_load
         * returned (cached per bank item), not the bank position */
        int ch = -1;
        if (slot >= 0)
            ch = is_music ? rt->sample_channel[64 + slot]
                          : rt->sample_channel[slot];
        if (ch >= 0) {
                    /* NuclearRT SampleActions (behavioural reference):
                     *   0 PlaySample, 4 PlaySample2, 11 PlaySampleChannel,
                     *     36 PlaySampleAllParameters  -> play (once)
                     *  -2 PlayAndLoopSample, 12 PlayAndLoopSampleAtChannel
                     *   -> play N times (param 2, 1-based count)
                     *   1 StopAnySample, 15 StopChannel -> stop
                     * The old code played on 0/1 (1 is STOP) and treated
                     * 4/36 as stop (they PLAY) — sounds went silent or
                     * never stopped.  Fusion loop counts are 1-based;
                     * our stream loop counter is 0-based (-1 infinite). */
                    switch (num) {
                    case 0: case 4: case 11:
                        if (rt->audio_play)
                            rt->audio_play(rt->audio, ch, 0);
                        break;
                    case -2: case 12: case 36: {
                        const fxp_param_t *pl = aparam_of(a, 2);
                        int32_t loops = 1;
                        if (num == -2)
                            pl = aparam_of(a, 1);
                        if (pl) {
                            fxp_value_t lv = param_expr_eval(frame, pl,
                                                             NULL, 0);
                            loops = fxp_value_as_int(&lv);
                            fxp_value_free(&lv);
                        }
                        if (rt->audio_play)
                            rt->audio_play(rt->audio, ch,
                                           loops > 0 ? loops - 1 : -1);
                        break;
                    }
                    case 1: case 15:
                        if (rt->audio_stop_all)
                            rt->audio_stop_all(rt->audio);
                        break;
                    default:
                        break;
                    }
        }
        free(name_tmp);
        return;
    }

    /* -------- text actions -------- */
    if (type == 3) {
        fxp_selector_t *sel = selector_by_oi(frame, oi);
        if (!sel)
            return;
        if (num == 88) {                  /* set alterable string */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t index = 0;
            if (p0 && !p0->has_expression)
                index = param_i16(p0, 0);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 1), obj,
                                                (uint32_t)oi);
                fxp_object_set_string(obj, index, fxp_value_as_string(&v));
                fxp_value_free(&v);
            }
        }
        /* 84/85/86 paragraph actions: not implemented yet */
        return;
    }

    /* -------- counter actions -------- */
    if (type == 7) {
        fxp_selector_t *sel = selector_by_oi(frame, oi);
        if (!sel)
            return;
        const fxp_param_t *p0 = aparam_of(a, 0);
        fxp_selector_iter_begin(sel);
        fxp_object_t *obj;
        while ((obj = fxp_selector_iter_next(sel)) != NULL) {
            fxp_value_t v = param_expr_eval(frame, p0, obj, (uint32_t)oi);
            fxp_value_t *cv = fxp_object_value(obj, 0);
            if (cv) {
                switch (num) {
                case 80: fxp_value_copy(cv, &v); break;
                case 81: fxp_value_add(cv, &v); break;
                case 82: fxp_value_sub(cv, &v); break;
                default: break;
                }
            }
            fxp_value_free(&v);
        }
        return;
    }

    /* -------- active / object actions -------- */
    if (type >= 0) {
        fxp_selector_t *sel = selector_by_oi(frame, oi);
        if (!sel)
            return;

        switch (num) {
        case 1: {                         /* Set position (Position param) */
            const fxp_param_t *p0 = aparam_of(a, 0);
            if (!p0 || p0->code != FXP_PARAM_POSITION)
                break;
            int32_t parent = param_u16(p0, 0);
            int32_t px = param_i16(p0, 4);
            int32_t py = param_i16(p0, 6);
            int32_t index = 0;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                if (parent == 0xFFFF) {
                    obj->x = px;
                    obj->y = py;
                } else {
                    fxp_object_t *parent_obj = NULL;
                    fxp_selector_t *psel = selector_by_oi(frame, parent);
                    if (psel) {
                        /* index-paired parent (nuclearrt At(it.index())) */
                        int32_t i = index < psel->selected_count
                                        ? index : psel->selected_count - 1;
                        parent_obj = i >= 0 ? psel->selected[i] : NULL;
                    }
                    if (parent_obj) {
                        obj->x = px + parent_obj->x;
                        obj->y = py + parent_obj->y;
                    }
                }
                index++;
            }
            break;
        }
        case 2: case 3: {                 /* Set X / Set Y */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                /* per-instance expressions: `X + 2` must read the value of
                 * the instance being moved, not the first selected one */
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), obj,
                                                (uint32_t)oi);
                int32_t val = fxp_value_as_int(&v);
                fxp_value_free(&v);
                if (num == 2)
                    obj->x = val;
                else
                    obj->y = val;
            }
            break;
        }
        case 24: {                        /* Destroy */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_frame_mark_for_deletion(frame, obj);
                fxp_selector_iter_deselect_current(sel);
                fxp_selector_remove(sel, obj);
            }
            break;
        }
        case 26: case 27: {               /* Hide / show */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->visible = (num == 27);
            break;
        }
        case 15: case 16: {               /* Anim stop / start */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->anim_stopped = (num == 15);
            break;
        }
        case 17: {                        /* Animation sequence */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            int32_t seq = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                obj->anim_seq = seq;
                obj->anim_frame = 0;
                obj->anim_clock = 0.0;
                obj->anim_over = false;
                obj->anim_stopped = false;
                obj->forced_dir = -1;
                obj->forced_frame = -1;
            }
            break;
        }
        case 18: {                        /* Force animation direction */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            int32_t dir = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                obj->forced_dir = dir;
                obj->anim_dir = dir;
                obj->anim_frame = 0;
                obj->anim_clock = 0.0;
                obj->anim_over = false;
            }
            break;
        }
        case 21: {                        /* Restore direction */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->forced_dir = -1;
            break;
        }
        case 40: {                        /* Force animation frame */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            int32_t f = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->forced_frame = f;
            break;
        }
        case 41: {                        /* Restore animation frame */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->forced_frame = -1;
            break;
        }
        case 4: case 5: {                 /* Movement stop / start */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                if (num == 4)
                    fxp_object_movement_stop(obj);
                else
                    fxp_object_movement_start(obj);
            }
            break;
        }
        case 6: {                         /* Set movement speed */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), obj,
                                                (uint32_t)oi);
                fxp_object_movement_set_speed(obj,
                                              fxp_value_as_int(&v));
                fxp_value_free(&v);
            }
            break;
        }
        case 9: {                         /* Bounce */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                fxp_object_movement_bounce(frame, obj);
            break;
        }
        case 11: case 12: {               /* Next / previous movement */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int32_t n = obj->item ? obj->item->common.movement_count : 0;
                if (n <= 0)
                    continue;
                int32_t idx = (int32_t)obj->movement_index + (num == 11 ? 1
                                                                       : -1);
                if (idx < 0)
                    idx = n - 1;
                if (idx >= n)
                    idx = 0;
                fxp_object_movement_select(frame, obj, idx);
            }
            break;
        }
        case 13: {                        /* Select movement */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            int32_t idx = fxp_value_as_int(&v);
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                fxp_object_movement_select(frame, obj, idx - 1);
            break;
        }
        case 23: {                        /* Set movement direction */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, p0, obj, (uint32_t)oi);
                int32_t val = fxp_value_as_int(&v);
                fxp_value_free(&v);
                obj->direction = val;
                if (obj->forced_dir >= 0 || obj->anim_dir != val) {
                    obj->anim_dir = val & 31;
                    obj->anim_frame = 0;
                }
            }
            break;
        }
        case 14: {                        /* Look at (direction toward) */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t other_oi = p0 ? param_u16(p0, 2) : -1;
            fxp_object_t *other = property_instance(frame, other_oi);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                if (!other)
                    continue;
                double dx = (double)(other->x - obj->x);
                double dy = (double)(other->y - obj->y);
                double ang = atan2(dy, dx) * 180.0 / 3.14159265358979;
                if (ang < 0)
                    ang += 360.0;
                obj->direction = (int32_t)((ang + 11.25) / 22.5) & 31;
            }
            break;
        }
        case 31: {                        /* Set alterable value */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t index = 0;
            if (p0 && !p0->has_expression)
                index = param_i16(p0, 0);
            else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                index = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
            }
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 1), obj,
                                                (uint32_t)oi);
                fxp_value_t *av = fxp_object_value(obj, index);
                if (av)
                    fxp_value_copy(av, &v);
                fxp_value_free(&v);
            }
            break;
        }
        case 32: case 33: {               /* Add / subtract alterable */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t index = 0;
            if (p0 && !p0->has_expression)
                index = param_i16(p0, 0);
            else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                index = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
            }
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 1), obj,
                                                (uint32_t)oi);
                fxp_value_t *av = fxp_object_value(obj, index);
                if (av) {
                    if (num == 32)
                        fxp_value_add(av, &v);
                    else
                        fxp_value_sub(av, &v);
                }
                fxp_value_free(&v);
            }
            break;
        }
        case 34: {                        /* Spread alterable value */
            const fxp_param_t *p0 = aparam_of(a, 0);
            int32_t index = 0;
            if (p0 && !p0->has_expression)
                index = param_i16(p0, 0);
            else {
                fxp_value_t e = param_expr_eval(frame, p0, NULL, 0);
                index = fxp_value_as_int(&e) - 1;
                fxp_value_free(&e);
            }
            fxp_value_t v = param_expr_eval(frame, aparam_of(a, 1), NULL, 0);
            int32_t first = fxp_value_as_int(&v);
            fxp_value_free(&v);
            int32_t k = 0;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t *av = fxp_object_value(obj, index);
                if (av)
                    fxp_value_init_int(av, first + k);
                k++;
            }
            break;
        }
        case 35: case 36: case 37: {      /* Set flag on/off/toggle */
            const fxp_param_t *p0 = aparam_of(a, 0);
            fxp_value_t v = param_expr_eval(frame, p0, NULL, 0);
            int32_t index = fxp_value_as_int(&v) - 1;
            fxp_value_free(&v);
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t *av = fxp_object_value(obj, index);
                if (!av)
                    continue;
                if (num == 35)
                    fxp_value_init_int(av, 1);
                else if (num == 36)
                    fxp_value_init_int(av, 0);
                else
                    fxp_value_init_int(av, fxp_value_as_int(av) ? 0 : 1);
            }
            break;
        }
        case 65: {                        /* Alpha coefficient */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), obj,
                                                (uint32_t)oi);
                obj->alpha_coefficient = (uint8_t)(fxp_value_as_int(&v) & 0xFF);
                fxp_value_free(&v);
            }
            break;
        }
        case 66: {                        /* RGB coefficient (0x00bbggrr) */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), obj,
                                                (uint32_t)oi);
                int32_t val = fxp_value_as_int(&v);
                fxp_value_free(&v);
                obj->tint_r = (uint8_t)(val & 0xFF);
                obj->tint_g = (uint8_t)((val >> 8) & 0xFF);
                obj->tint_b = (uint8_t)((val >> 16) & 0xFF);
                obj->has_tint = true;
            }
            break;
        }
        case 63: case 64:                 /* effect / effect parameter */
        case 85: case 86: case 87:        /* scale */
        case 88: {                        /* angle */
            if (num == 88) {
                fxp_selector_iter_begin(sel);
                fxp_object_t *obj;
                while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                    fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0),
                                                    obj, (uint32_t)oi);
                    obj->angle = (uint32_t)(fxp_value_as_int(&v) & 0xFF);
                    fxp_value_free(&v);
                }
            }
            break;
        }
        case 57: case 58: {               /* Move to front / back */
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int32_t idx = -1;
                for (int32_t i = 0; i < frame->instance_count; i++)
                    if (frame->instances[i] == obj) {
                        idx = i;
                        break;
                    }
                if (idx < 0)
                    continue;
                memmove(&frame->instances[idx], &frame->instances[idx + 1],
                        (size_t)(frame->instance_count - idx - 1) *
                            sizeof(fxp_object_t *));
                if (num == 57)
                    frame->instances[frame->instance_count - 1] = obj;
                else {
                    /* move to back: shift one slot right (count-1 elements,
                     * not count — the old code wrote one pointer PAST the
                     * end of the instance array) */
                    memmove(&frame->instances[1], &frame->instances[0],
                            (size_t)(frame->instance_count - 1) *
                                sizeof(fxp_object_t *));
                    frame->instances[0] = obj;
                }
            }
            break;
        }
        case 61: {                        /* Move to layer */
            fxp_value_t v = param_expr_eval(frame, aparam_of(a, 0), NULL, 0);
            int32_t val = fxp_value_as_int(&v) - 1;
            fxp_value_free(&v);
            if (val < 0)
                val = 0;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL)
                obj->layer = (uint32_t)val;
            break;
        }
        case 29: case 30: {               /* Shoot */
            const fxp_param_t *p0 = aparam_of(a, 0);
            if (!p0 || p0->code != FXP_PARAM_SHOOT)
                break;
            int32_t new_oi = param_u16(p0, 26);
            fxp_item_t *item = fxp_mfa_frame_item(frame->def, new_oi);
            if (!item)
                break;
            fxp_selector_iter_begin(sel);
            fxp_object_t *obj;
            while ((obj = fxp_selector_iter_next(sel)) != NULL) {
                int32_t img = object_current_image_handle(obj);
                const fxp_image_t *im = image_by_handle(frame, img);
                int32_t sx = obj->x + (im ? im->action_x : 0);
                int32_t sy = obj->y + (im ? im->action_y : 0);
                double ang = (double)((param_i32(p0, 12) & 0xFF));
                fxp_value_t spv = param_expr_eval(frame, aparam_of(a, 1), obj,
                                                  (uint32_t)oi);
                int32_t speed = fxp_value_as_int(&spv);
                fxp_value_free(&spv);
                fxp_object_t *proj = fxp_frame_create_instance(frame, item,
                                                               sx, sy,
                                                               (int32_t)obj->layer);
                if (proj) {
                    proj->direction = (int32_t)(ang / 11.25) & 31;
                    proj->speed = (double)speed;
                    fxp_selector_t *psel = selector_by_oi(frame, new_oi);
                    if (psel) {
                        fxp_selector_add(psel, proj);
                        fxp_selector_select_only(psel, proj);
                    }
                }
            }
            break;
        }
        default:
            warn_once(type, num, "object action");
            break;
        }
    }
}

/* ==================================================== group execution === */

/* Evaluate conditions [from, to) as one OR-branch. */
static bool eval_segment(fxp_frame_t *frame, int32_t gi,
                         const fxp_event_group_t *g, int32_t from, int32_t to,
                         const int32_t *evois, bool *evused, int32_t evcount,
                         fxp_object_t *true_source)
{
    fxp_runtime_t *rt = frame->rt;
    for (int32_t ci = from; ci < to; ci++) {
        const fxp_condition_t *c = &g->conditions[ci];

        /* reset selectors referenced by this condition (first use) */
        int32_t ois[64];
        int32_t n = collect_cond_ois(frame, c, ois, 64);
        for (int32_t i = 0; i < n; i++) {
            for (int32_t k = 0; k < evcount; k++) {
                if (evois[k] == ois[i]) {
                    if (!evused[k]) {
                        fxp_selector_t *sel = selector_by_oi(frame, ois[i]);
                        if (sel)
                            fxp_selector_reset(sel, true_source);
                        evused[k] = true;
                    }
                    break;
                }
            }
        }

        bool result;
        if (c->object_type == -6 && c->num == -7) {
            /* click on object: needs the click EDGE *and* the mouse over
             * the instance (was hover-only before) */
            result = rt->input.mouse_clicked &&
                     filter_under_mouse(frame, (int32_t)c->object_info);
        } else if (c->object_type == -6 && c->num == -4) {
            result = filter_under_mouse(frame, (int32_t)c->object_info);
        } else {
            result = eval_condition(frame, gi, c, true_source);
        }

        bool negated = (c->other_flags & 1) != 0;
        if (negated)
            result = !result;
        if (!result)
            return false;
    }
    return true;
}

static void exec_group(fxp_frame_t *frame, int32_t gi, fxp_object_t *true_source)
{
    const fxp_mfa_frame_t *def = frame->def;
    fxp_runtime_t *rt = frame->rt;
    fxp_event_group_t *g = &def->events.groups[gi];

    if (g->flags & 0x4000)                /* inactive row */
        return;
    if (rt->gowner[gi] > 0 && !group_is_active(frame, rt->gowner[gi]))
        return;

    group_state_ensure(frame, def->events.group_count);

    /* collect every OI referenced by conditions + actions */
    int32_t ois[128];
    int32_t n = 0;
    for (int32_t c = 0; c < g->condition_count && n < 128; c++) {
        int32_t t[64];
        int32_t k = collect_cond_ois(frame, &g->conditions[c], t, 64);
        for (int32_t i = 0; i < k && n < 128; i++) {
            bool have = false;
            for (int32_t j = 0; j < n; j++)
                if (ois[j] == t[i])
                    have = true;
            if (!have)
                ois[n++] = t[i];
        }
    }
    for (int32_t a = 0; a < g->action_count && n < 128; a++) {
        int32_t t[64];
        int32_t k = collect_action_ois(frame, &g->actions[a], t, 64);
        for (int32_t i = 0; i < k && n < 128; i++) {
            bool have = false;
            for (int32_t j = 0; j < n; j++)
                if (ois[j] == t[i])
                    have = true;
            if (!have)
                ois[n++] = t[i];
        }
    }

    /* entry snapshots for OR-branch retries */
    fxp_object_t **snaps[128] = { 0 };
    int32_t snap_counts[128] = { 0 };
    for (int32_t i = 0; i < n; i++) {
        fxp_selector_t *sel = selector_by_oi(frame, ois[i]);
        if (sel)
            fxp_selector_save(sel, &snaps[i], &snap_counts[i]);
    }

    bool used[128] = { false };

    /* conditions with OR-ladder (nuclearrt GenerateEventNextLabel) */
    bool passed = false;
    int32_t seg_start = 0;
    int32_t i = 0;
    int32_t cond_count = g->condition_count;
    while (i <= cond_count) {
        bool is_or = i < cond_count &&
                     g->conditions[i].object_type == -1 &&
                     g->conditions[i].num == -25;
        if (i == cond_count || is_or) {
            if (eval_segment(frame, gi, g, seg_start, i, ois, used, n,
                             true_source)) {
                passed = true;
                break;
            }
            /* restore entry selections before next branch */
            for (int32_t k = 0; k < n; k++) {
                fxp_selector_t *sel = selector_by_oi(frame, ois[k]);
                if (sel && snaps[k])
                    fxp_selector_restore(sel, snaps[k], snap_counts[k]);
                else if (sel)
                    fxp_selector_reset(sel, NULL);
            }
            seg_start = i + 1;
        }
        i++;
    }

    if (passed) {
        /* one-action-loop marker: skip actions every other tick */
        bool has_one_action_loop = false;
        for (int32_t c = 0; c < cond_count; c++)
            if (g->conditions[c].object_type == -1 &&
                g->conditions[c].num == -7)
                has_one_action_loop = true;

        bool skip_actions = false;
        if (has_one_action_loop) {
            if (rt->action_ran_last[gi])
                skip_actions = true;
            rt->action_ran_last[gi] = true;
        }

        if (!skip_actions) {
            for (int32_t a = 0; a < g->action_count; a++) {
                fxp_action_t *act = &g->actions[a];
                int32_t aois[64];
                int32_t an = collect_action_ois(frame, act, aois, 64);
                for (int32_t i = 0; i < an; i++) {
                    for (int32_t k = 0; k < n; k++) {
                        if (ois[k] == aois[i]) {
                            if (!used[k]) {
                                fxp_selector_t *sel =
                                    selector_by_oi(frame, aois[i]);
                                if (sel)
                                    fxp_selector_reset(sel, NULL);
                                used[k] = true;
                            }
                            break;
                        }
                    }
                }
                exec_action(frame, act);
            }
        }
    } else if (i >= cond_count) {
        /* all branches failed: reset one-action-loop latch */
        for (int32_t c = 0; c < cond_count; c++)
            if (g->conditions[c].object_type == -1 &&
                g->conditions[c].num == -7)
                rt->action_ran_last[gi] = false;
    }

    for (int32_t k = 0; k < n; k++)
        fxp_selector_free_snapshot(snaps[k]);
}

/* ================================================== event passes ======== */

static void run_event_pass(fxp_frame_t *frame, int pass)
{
    const fxp_mfa_frame_t *def = frame->def;
    const fxp_events_t *ev = &def->events;
    fxp_runtime_t *rt = frame->rt;
    if (!rt->gclass)
        groups_classify(frame);

    for (int32_t gi = 0; gi < ev->group_count; gi++) {
        uint8_t cls = rt->gclass[gi];
        if (cls == FXP_GC_MARKER)
            continue;

        switch (pass) {
        case FXP_PASS_TIMER:
            if (cls != FXP_GC_TIMER)
                continue;
            break;
        case FXP_PASS_NORMAL:
            if (cls != FXP_GC_NORMAL)
                continue;
            break;
        case FXP_PASS_EOF:
            if (cls != FXP_GC_EOF)
                continue;
            break;
        case FXP_PASS_LOOP:
            if (cls != FXP_GC_LOOP)
                continue;
            break;
        case FXP_PASS_NAMED:
            if (cls != FXP_GC_NAMED)
                continue;
            break;
        default:
            continue;
        }
        exec_group(frame, gi, NULL);
    }
}

/* True-event dispatch: run groups whose first condition matches
 * (type, num) — Frame::GenerateEvent. */
static void dispatch_event(fxp_frame_t *frame, int32_t type, int32_t num,
                           fxp_object_t *source)
{
    const fxp_mfa_frame_t *def = frame->def;
    const fxp_events_t *ev = &def->events;
    fxp_runtime_t *rt = frame->rt;
    if (!rt->gclass)
        groups_classify(frame);

    for (int32_t gi = 0; gi < ev->group_count; gi++) {
        if (rt->gclass[gi] != FXP_GC_TRUE && rt->gclass[gi] != FXP_GC_SOF)
            continue;
        const fxp_event_group_t *g = &ev->groups[gi];
        if (g->condition_count == 0)
            continue;
        const fxp_condition_t *c0 = &g->conditions[0];
        if (c0->object_type != type || c0->num != num)
            continue;
        exec_group(frame, gi, source);
    }
}

static void dispatch_true_events(fxp_frame_t *frame)
{
    fxp_runtime_t *rt = frame->rt;

    if (rt->update_count == 1)
        dispatch_event(frame, -3, -1, NULL);      /* start of frame */

    if (rt->input.any_key_pressed)
        dispatch_event(frame, -6, -9, NULL);      /* any key */

    if (rt->input.mouse_clicked) {
        dispatch_event(frame, -6, -5, NULL);      /* clicking */
        dispatch_event(frame, -6, -6, NULL);      /* clicking in zone */
        dispatch_event(frame, -6, -7, NULL);      /* clicking on object */
    }

    if (any_control_pressed(frame))
        dispatch_event(frame, -7, -4, NULL);      /* control pressed */
}

/* ======================================================== tick pipeline = */

void fxp_runtime_tick(fxp_frame_t *frame, double dt)
{
    fxp_runtime_t *rt = frame->rt;
    if (!rt)
        return;

    rt->update_count++;
    rt->time += dt * 1000.0;

    /* advance per-event timer accumulators; clear per-frame check flags */
    for (int32_t i = 0; i < rt->timer_event_count; i++) {
        rt->timer_events[i].time += dt * 1000.0;
        rt->timer_events[i].already_checked = false;
    }

    if (!rt->gclass)
        groups_classify(frame);

    /* 1. true events (Frame::DispatchTrueEvents) */
    dispatch_true_events(frame);

    /* 2. generated object events (animation over, ...) */
    int32_t pd = 0;
    while (pd < rt->pending_count) {
        dispatch_event(frame, rt->pending[pd].type, rt->pending[pd].num,
                       rt->pending[pd].source);
        pd++;
    }
    rt->pending_count = 0;

    /* 3. timer-condition events (before the normal loop) */
    run_event_pass(frame, FXP_PASS_TIMER);

    /* 4. named timer events */
    process_named_timers(frame);

    /* 5. normal event loop */
    run_event_pass(frame, FXP_PASS_NORMAL);

    /* 6. end-of-frame pass */
    frame->at_eof = true;
    run_event_pass(frame, FXP_PASS_EOF);
    frame->at_eof = false;
}

void fxp_runtime_end_tick(fxp_runtime_t *rt)
{
    if (!rt)
        return;
    memset(rt->input.controls_pressed, 0, sizeof(rt->input.controls_pressed));
    memset(rt->input.keys_pressed, 0, sizeof(rt->input.keys_pressed));
    rt->input.any_key_pressed = false;
    rt->input.mouse_clicked = false;
}
