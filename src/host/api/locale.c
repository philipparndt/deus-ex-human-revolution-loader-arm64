// <locale.h>: the host process locale is the guest's locale.
//
// i386 struct lconv (56 bytes): ten char* (decimal_point, thousands_sep,
// grouping, int_curr_symbol, currency_symbol, mon_decimal_point,
// mon_thousands_sep, mon_grouping, positive_sign, negative_sign) @0..39, then
// the char fields int_frac_digits..n_sign_posn @40..47 and
// int_p_cs_precedes..int_n_sign_posn @48..53.
#include "common.h"
#include "libc.h"

#include <locale.h>
#include <pthread.h>
#include <stdlib.h>

static void update_mb_cur_max(void) {
    gaddr_t a = hostsym_lookup("___mb_cur_max");
    if (a) gwrite32(a, (uint32_t)MB_CUR_MAX);
}

THUNK(setlocale) {
    const char *s = setlocale(ARGI(0), ARG(1) ? ARGS(1) : NULL);
    if (!s) {
        RET(0);
        return;
    }
    if (ARG(1)) {
        LOGD("setlocale(%d, \"%s\") -> %s", ARGI(0), ARGS(1), s);
        update_mb_cur_max();
    }
    RET(gintern(s));
}

static gaddr_t lconv_buf;
static pthread_mutex_t lconv_lock = PTHREAD_MUTEX_INITIALIZER;

THUNK(localeconv) {
    pthread_mutex_lock(&lconv_lock);
    if (!lconv_buf) lconv_buf = gheap_calloc(1, 56);
    const struct lconv *lc = localeconv();
    const char *strs[10] = {lc->decimal_point,     lc->thousands_sep, lc->grouping,      lc->int_curr_symbol,
                            lc->currency_symbol,   lc->mon_decimal_point, lc->mon_thousands_sep,
                            lc->mon_grouping,      lc->positive_sign, lc->negative_sign};
    for (int i = 0; i < 10; i++) gwrite32(lconv_buf + 4 * (uint32_t)i, gintern(strs[i] ? strs[i] : ""));
    const char c1[8] = {lc->int_frac_digits, lc->frac_digits,    lc->p_cs_precedes, lc->p_sep_by_space,
                        lc->n_cs_precedes,   lc->n_sep_by_space, lc->p_sign_posn,   lc->n_sign_posn};
    const char c2[6] = {lc->int_p_cs_precedes,  lc->int_n_cs_precedes, lc->int_p_sep_by_space,
                        lc->int_n_sep_by_space, lc->int_p_sign_posn,   lc->int_n_sign_posn};
    memcpy(g2h(lconv_buf + 40), c1, 8);
    memcpy(g2h(lconv_buf + 48), c2, 6);
    pthread_mutex_unlock(&lconv_lock);
    RET(lconv_buf);
}
