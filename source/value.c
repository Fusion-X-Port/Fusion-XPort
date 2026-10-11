/*
 * Fusion X-Port — value.c
 * Tagged expression value with Clickteam semantics.
 */
#include "fxp/value.h"

#include <math.h>

/* Rotating scratch buffers for numeric->string conversion.  A single
 * static buffer was clobbered when one expression held two conversions at
 * once (Find/ReverseFind convert both args through fxp_value_as_string —
 * the second call overwrote the first pointer). */
#define FXP_NUMBUF_SLOTS 8
#define FXP_NUMBUF_SIZE  64

const char *fxp_value_as_string(const fxp_value_t *v)
{
    static char bufs[FXP_NUMBUF_SLOTS][FXP_NUMBUF_SIZE];
    static int slot;
    char *buf = bufs[slot];
    slot = (slot + 1) % FXP_NUMBUF_SLOTS;
    if (v->type == FXP_VAL_STRING)
        return v->as.s;
    if (v->type == FXP_VAL_DOUBLE)
        snprintf(buf, FXP_NUMBUF_SIZE, "%g", v->as.d);
    else
        snprintf(buf, FXP_NUMBUF_SIZE, "%d", v->as.i);
    return buf;
}

void fxp_value_init_int(fxp_value_t *v, int32_t i)
{
    v->type   = FXP_VAL_INT;
    v->as.i   = i;
}

void fxp_value_init_double(fxp_value_t *v, double d)
{
    v->type   = FXP_VAL_DOUBLE;
    v->as.d   = d;
}

void fxp_value_init_string(fxp_value_t *v, const char *s)
{
    v->type      = FXP_VAL_STRING;
    v->as.s      = fxp_strdup(s ? s : "");
}

void fxp_value_free(fxp_value_t *v)
{
    if (v->type == FXP_VAL_STRING) {
        free(v->as.s);
        v->as.s = NULL;
    }
    v->type = FXP_VAL_INT;
    v->as.i = 0;
}

void fxp_value_copy(fxp_value_t *dst, const fxp_value_t *src)
{
    if (dst == src)
        return;
    fxp_value_free(dst);
    if (src->type == FXP_VAL_STRING) {
        dst->type = FXP_VAL_STRING;
        dst->as.s = fxp_strdup(src->as.s);
    } else {
        dst->type = src->type;
        dst->as   = src->as;
    }
}

int32_t fxp_value_as_int(const fxp_value_t *v)
{
    switch (v->type) {
    case FXP_VAL_INT:    return v->as.i;
    case FXP_VAL_DOUBLE: return (int32_t)v->as.d;
    default:             return (int32_t)strtol(v->as.s, NULL, 10);
    }
}

double fxp_value_as_double(const fxp_value_t *v)
{
    switch (v->type) {
    case FXP_VAL_INT:    return (double)v->as.i;
    case FXP_VAL_DOUBLE: return v->as.d;
    default:             return strtod(v->as.s, NULL);
    }
}

/* Type promotion: if either side is a string or double the result is a
 * double (strings must both be numeric via strtod — Clickteam quirk). */
static void promote(fxp_value_t *a, const fxp_value_t *b, double *da, double *db)
{
    *da = fxp_value_as_double(a);
    *db = fxp_value_as_double(b);
    if (a->type == FXP_VAL_INT && b->type == FXP_VAL_INT)
        return;
    /* result becomes double */
    double d = *da;
    fxp_value_free(a);
    a->type = FXP_VAL_DOUBLE;
    a->as.d = d;
}

void fxp_value_add(fxp_value_t *a, const fxp_value_t *b)
{
    if (a->type == FXP_VAL_STRING || b->type == FXP_VAL_STRING) {
        /* string concatenation (int+string in Fusion concatenates) */
        size_t la = strlen(fxp_value_as_string(a));
        size_t lb = strlen(fxp_value_as_string(b));
        char *s = fxp_malloc(la + lb + 1);
        memcpy(s, fxp_value_as_string(a), la);
        memcpy(s + la, fxp_value_as_string(b), lb);
        s[la + lb] = '\0';
        fxp_value_free(a);
        a->type = FXP_VAL_STRING;
        a->as.s = s;
        return;
    }
    double da, db;
    promote(a, b, &da, &db);
    if (a->type == FXP_VAL_DOUBLE) a->as.d = da + db;
    else                           a->as.i = (int32_t)(fxp_value_as_int(a) + fxp_value_as_int(b));
}

void fxp_value_sub(fxp_value_t *a, const fxp_value_t *b)
{
    double da, db;
    promote(a, b, &da, &db);
    if (a->type == FXP_VAL_DOUBLE) a->as.d = da - db;
    else                           a->as.i = (int32_t)(fxp_value_as_int(a) - fxp_value_as_int(b));
}

void fxp_value_mul(fxp_value_t *a, const fxp_value_t *b)
{
    double da, db;
    promote(a, b, &da, &db);
    if (a->type == FXP_VAL_DOUBLE) a->as.d = da * db;
    else                           a->as.i = (int32_t)((int64_t)fxp_value_as_int(a) * fxp_value_as_int(b));
}

void fxp_value_div(fxp_value_t *a, const fxp_value_t *b)
{
    double da, db;
    promote(a, b, &da, &db);
    /* Clickteam semantics: divide by zero yields 0 */
    if (a->type == FXP_VAL_DOUBLE) a->as.d = (db == 0.0) ? 0.0 : da / db;
    else {
        int32_t ib = fxp_value_as_int(b);
        a->as.i = (ib == 0) ? 0 : (int32_t)(fxp_value_as_int(a) / ib);
    }
}

void fxp_value_mod(fxp_value_t *a, const fxp_value_t *b)
{
    double da, db;
    promote(a, b, &da, &db);
    if (a->type == FXP_VAL_DOUBLE) a->as.d = (db == 0.0) ? 0.0 : fmod(da, db);
    else {
        int32_t ib = fxp_value_as_int(b);
        a->as.i = (ib == 0) ? 0 : (int32_t)(fxp_value_as_int(a) % ib);
    }
}

int fxp_value_cmp(const fxp_value_t *a, const fxp_value_t *b)
{
    if (a->type == FXP_VAL_STRING && b->type == FXP_VAL_STRING)
        return strcmp(a->as.s, b->as.s);
    double da = fxp_value_as_double(a);
    double db = fxp_value_as_double(b);
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}
