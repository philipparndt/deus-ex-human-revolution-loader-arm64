// libm and the compiler-rt helpers an i386 compiler emits calls to.
//
// double arguments take two slots (ARGD), float arguments one (ARGF); all
// floating point results are returned in st(0) via RETD. 80-bit long double
// arguments occupy 16 bytes on the stack and are converted to double.
#include "common.h"
#include "libc.h"

#include <math.h>
#include <stdint.h>

#define LD_ARG(i) x87_to_double(g2h(r->esp + 4 + 4 * (i)))

// --- one/two argument functions ------------------------------------------------
#define MATH_D1(fn) THUNK(fn) { RETD(fn(ARGD(0))); }
#define MATH_F1(fn) THUNK(fn) { RETD(fn(ARGF(0))); }
#define MATH_L1(fn, dfn) THUNK(fn) { RETD(dfn(LD_ARG(0))); }
#define MATH_D2(fn) THUNK(fn) { RETD(fn(ARGD(0), ARGD(2))); }
#define MATH_F2(fn) THUNK(fn) { RETD(fn(ARGF(0), ARGF(1))); }

#define MATH_DF1(name) MATH_D1(name) MATH_F1(name##f)
#define MATH_DF2(name) MATH_D2(name) MATH_F2(name##f)

MATH_DF1(sin) MATH_DF1(cos) MATH_DF1(tan)
MATH_DF1(asin) MATH_DF1(acos) MATH_DF1(atan)
MATH_DF1(sinh) MATH_DF1(cosh) MATH_DF1(tanh)
MATH_DF1(asinh) MATH_DF1(acosh) MATH_DF1(atanh)
MATH_DF1(exp) MATH_DF1(exp2) MATH_DF1(expm1)
MATH_DF1(log) MATH_DF1(log10) MATH_DF1(log2) MATH_DF1(log1p) MATH_DF1(logb)
MATH_DF1(sqrt) MATH_DF1(cbrt)
MATH_DF1(fabs) MATH_DF1(ceil) MATH_DF1(floor) MATH_DF1(trunc) MATH_DF1(round)
MATH_DF1(rint) MATH_DF1(nearbyint)
MATH_DF1(erf) MATH_DF1(erfc) MATH_DF1(tgamma) MATH_DF1(lgamma)

MATH_DF2(atan2) MATH_DF2(pow) MATH_DF2(fmod) MATH_DF2(hypot) MATH_DF2(copysign)
MATH_DF2(fmin) MATH_DF2(fmax) MATH_DF2(fdim) MATH_DF2(remainder) MATH_DF2(nextafter)

MATH_L1(sqrtl, sqrt) MATH_L1(fabsl, fabs) MATH_L1(floorl, floor) MATH_L1(ceill, ceil)
MATH_L1(sinl, sin) MATH_L1(cosl, cos) MATH_L1(expl, exp) MATH_L1(logl, log)

THUNK(fma) { RETD(fma(ARGD(0), ARGD(2), ARGD(4))); }
THUNK(fmaf) { RETD(fmaf(ARGF(0), ARGF(1), ARGF(2))); }
THUNK(powl) { RETD(pow(LD_ARG(0), LD_ARG(4))); }
THUNK(fmodl) { RETD(fmod(LD_ARG(0), LD_ARG(4))); }

// --- functions with integer or pointer arguments ---------------------------------------
THUNK(modf) {
    double ip;
    double fr = modf(ARGD(0), &ip);
    if (ARG(2)) memcpy(g2h(ARG(2)), &ip, 8);
    RETD(fr);
}
THUNK(modff) {
    float ip;
    float fr = modff(ARGF(0), &ip);
    if (ARG(1)) memcpy(g2h(ARG(1)), &ip, 4);
    RETD(fr);
}
THUNK(frexp) {
    int e = 0;
    double m = frexp(ARGD(0), &e);
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)e);
    RETD(m);
}
THUNK(frexpf) {
    int e = 0;
    float m = frexpf(ARGF(0), &e);
    if (ARG(1)) gwrite32(ARG(1), (uint32_t)e);
    RETD(m);
}
THUNK(remquo) {
    int q = 0;
    double v = remquo(ARGD(0), ARGD(2), &q);
    if (ARG(4)) gwrite32(ARG(4), (uint32_t)q);
    RETD(v);
}
THUNK(remquof) {
    int q = 0;
    float v = remquof(ARGF(0), ARGF(1), &q);
    if (ARG(2)) gwrite32(ARG(2), (uint32_t)q);
    RETD(v);
}
THUNK(ldexp) { RETD(ldexp(ARGD(0), ARGI(2))); }
THUNK(ldexpf) { RETD(ldexpf(ARGF(0), ARGI(1))); }
THUNK(scalbn) { RETD(scalbn(ARGD(0), ARGI(2))); }
THUNK(scalbnf) { RETD(scalbnf(ARGF(0), ARGI(1))); }
THUNK(ilogb) { RET(ilogb(ARGD(0))); }
THUNK(ilogbf) { RET(ilogbf(ARGF(0))); }

// long is 32-bit in the guest: out-of-range results are unspecified; clamp like x87 (0x80000000).
static uint32_t to_glong(double v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
THUNK(lround) { RET(to_glong(round(ARGD(0)))); }
THUNK(lroundf) { RET(to_glong(roundf(ARGF(0)))); }
THUNK(lrint) { RET(to_glong(rint(ARGD(0)))); }
THUNK(lrintf) { RET(to_glong(rintf(ARGF(0)))); }
THUNK(llround) { RET64(llround(ARGD(0))); }
THUNK(llroundf) { RET64(llroundf(ARGF(0))); }
THUNK(llrint) { RET64(llrint(ARGD(0))); }
THUNK(llrintf) { RET64(llrintf(ARGF(0))); }
THUNK(nan) { RETD(nan(ARG(0) ? ARGS(0) : "")); }
THUNK(nanf) { RETD(nanf(ARG(0) ? ARGS(0) : "")); }

// --- classification helpers used by the <math.h> macros -----------------------------------
THUNK(__isnand) { RET(isnan(ARGD(0)) ? 1 : 0); }
THUNK(__isnanf) { RET(isnan(ARGF(0)) ? 1 : 0); }
THUNK(__isnanl) { RET(isnan(LD_ARG(0)) ? 1 : 0); }
THUNK(__isinfd) { RET(isinf(ARGD(0)) ? 1 : 0); }
THUNK(__isinff) { RET(isinf(ARGF(0)) ? 1 : 0); }
THUNK(__isinfl) { RET(isinf(LD_ARG(0)) ? 1 : 0); }
THUNK(__isfinited) { RET(isfinite(ARGD(0)) ? 1 : 0); }
THUNK(__isfinitef) { RET(isfinite(ARGF(0)) ? 1 : 0); }
THUNK(__isfinitel) { RET(isfinite(LD_ARG(0)) ? 1 : 0); }
THUNK(__isnormald) { RET(isnormal(ARGD(0)) ? 1 : 0); }
THUNK(__isnormalf) { RET(isnormal(ARGF(0)) ? 1 : 0); }
THUNK(__signbitd) { RET(signbit(ARGD(0)) ? 1 : 0); }
THUNK(__signbitf) { RET(signbit(ARGF(0)) ? 1 : 0); }
THUNK(__fpclassifyd) { RET(fpclassify(ARGD(0))); }
THUNK(__fpclassifyf) { RET(fpclassify(ARGF(0))); }
THUNK(__fpclassify) { RET(fpclassify(LD_ARG(0))); }

// --- compiler-rt: 64-bit integer arithmetic ---------------------------------------------
// Division by zero is undefined; return 0 instead of trapping in the host.
THUNK(__divdi3) {
    int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2);
    if (b == 0) RET64(0);
    else if (b == -1) RET64(0 - (uint64_t)a); // INT64_MIN / -1 wraps
    else RET64(a / b);
}
THUNK(__moddi3) {
    int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2);
    if (b == 0 || b == -1) RET64(0);
    else RET64(a % b);
}
THUNK(__udivdi3) {
    uint64_t a = ARG64(0), b = ARG64(2);
    RET64(b ? a / b : 0);
}
THUNK(__umoddi3) {
    uint64_t a = ARG64(0), b = ARG64(2);
    RET64(b ? a % b : 0);
}
THUNK(__udivmoddi4) {
    uint64_t a = ARG64(0), b = ARG64(2);
    uint64_t q = b ? a / b : 0, m = b ? a % b : 0;
    if (ARG(4)) gwrite64(ARG(4), m);
    RET64(q);
}
THUNK(__divmoddi4) {
    int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2);
    int64_t q = 0, m = 0;
    if (b == -1) q = (int64_t)(0 - (uint64_t)a);
    else if (b) { q = a / b; m = a % b; }
    if (ARG(4)) gwrite64(ARG(4), (uint64_t)m);
    RET64(q);
}
THUNK(__muldi3) { RET64(ARG64(0) * ARG64(2)); }
THUNK(__negdi2) { RET64(0 - ARG64(0)); }
THUNK(__ashldi3) { RET64(ARG64(0) << (ARG(2) & 63)); }
THUNK(__lshrdi3) { RET64(ARG64(0) >> (ARG(2) & 63)); }
THUNK(__ashrdi3) { RET64((uint64_t)((int64_t)ARG64(0) >> (ARG(2) & 63))); }
THUNK(__cmpdi2) {
    int64_t a = (int64_t)ARG64(0), b = (int64_t)ARG64(2);
    RET(a < b ? 0 : a > b ? 2 : 1);
}
THUNK(__ucmpdi2) {
    uint64_t a = ARG64(0), b = ARG64(2);
    RET(a < b ? 0 : a > b ? 2 : 1);
}

// --- compiler-rt: float <-> 64-bit integer conversions ---------------------------------------
// Unsigned conversions follow compiler-rt: negative or < 1 -> 0, too large (or +NaN) -> all ones.
static uint64_t fixuns(double a, bool neg_sign) {
    if (neg_sign || a < 1.0) return 0;
    if (!(a < 18446744073709551616.0)) return UINT64_MAX;
    return (uint64_t)a;
}
static int64_t fixs(double a) {
    if (isnan(a)) return INT64_MIN; // compiler-rt: exponent overflow path
    if (a >= 9223372036854775808.0) return INT64_MAX;
    if (a < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)a;
}
THUNK(__fixunsdfdi) {
    double d = ARGD(0);
    RET64(fixuns(d, signbit(d)));
}
THUNK(__fixunssfdi) {
    float f = ARGF(0);
    RET64(fixuns(f, signbit(f)));
}
THUNK(__fixunsxfdi) {
    double d = LD_ARG(0);
    RET64(fixuns(d, signbit(d)));
}
THUNK(__fixdfdi) { RET64((uint64_t)fixs(ARGD(0))); }
THUNK(__fixsfdi) { RET64((uint64_t)fixs(ARGF(0))); }
THUNK(__fixxfdi) { RET64((uint64_t)fixs(LD_ARG(0))); }
THUNK(__floatdidf) { RETD((double)(int64_t)ARG64(0)); }
THUNK(__floatundidf) { RETD((double)ARG64(0)); }
THUNK(__floatdisf) { RETD((float)(int64_t)ARG64(0)); }
THUNK(__floatundisf) { RETD((float)ARG64(0)); }
THUNK(__floatdixf) { RETD((double)(int64_t)ARG64(0)); }
THUNK(__floatundixf) { RETD((double)ARG64(0)); }
