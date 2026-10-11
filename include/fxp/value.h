/*
 * Fusion X-Port — value.h
 * Fusion expression value: tagged int / double / string.
 *
 * Semantics ported from NuclearRT's CValue (runtime/include/CValue.h):
 * Clickteam semantics — division/modulo by zero yields 0, int+double
 * promotes to double, int+string concatenates the decimal form.
 */
#ifndef FXP_VALUE_H
#define FXP_VALUE_H

#include "fxp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FXP_VAL_INT    = 0,
    FXP_VAL_DOUBLE = 1,
    FXP_VAL_STRING = 2,
};

typedef struct fxp_value {
    uint8_t type;
    union {
        int32_t i;
        double  d;
        char   *s;      /* owned when type == STRING */
    } as;
} fxp_value_t;

void  fxp_value_init_int(fxp_value_t *v, int32_t i);
void  fxp_value_init_double(fxp_value_t *v, double d);
void  fxp_value_init_string(fxp_value_t *v, const char *s);   /* copies */
void  fxp_value_free(fxp_value_t *v);
void  fxp_value_copy(fxp_value_t *dst, const fxp_value_t *src);

int32_t    fxp_value_as_int(const fxp_value_t *v);
double     fxp_value_as_double(const fxp_value_t *v);
const char *fxp_value_as_string(const fxp_value_t *v);        /* static buf for non-strings */

void fxp_value_add(fxp_value_t *a, const fxp_value_t *b);
void fxp_value_sub(fxp_value_t *a, const fxp_value_t *b);
void fxp_value_mul(fxp_value_t *a, const fxp_value_t *b);
void fxp_value_div(fxp_value_t *a, const fxp_value_t *b);
void fxp_value_mod(fxp_value_t *a, const fxp_value_t *b);
/* Three-way comparison: <0, 0, >0 */
int  fxp_value_cmp(const fxp_value_t *a, const fxp_value_t *b);

#ifdef __cplusplus
}
#endif
#endif /* FXP_VALUE_H */
