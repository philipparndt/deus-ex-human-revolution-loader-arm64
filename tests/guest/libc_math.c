// libm and compiler-rt helpers.
#include <math.h>
#include <stdint.h>
#include "libc_check.h"

extern int __isnand(double), __isnanf(float), __isinfd(double), __isinff(float), __isfinitef(float);
extern uint64_t __fixunsdfdi(double), __fixunssfdi(float);
extern int64_t __divdi3(int64_t, int64_t), __moddi3(int64_t, int64_t);
extern uint64_t __udivdi3(uint64_t, uint64_t), __umoddi3(uint64_t, uint64_t);
extern float ceilf(float), floorf(float), exp2f(float), log10f(float), roundf(float), rintf(float);

static int near(double a, double b) { return fabs(a - b) < 1e-9 * (fabs(b) + 1); }
static int nearf(float a, float b) { return fabsf(a - b) < 1e-5f * (fabsf(b) + 1); }

int main(void) {
    volatile double x = 0.5, y = 2.0, big = 1e19, neg = -3.7;
    volatile float xf = 0.5f, yf = 2.0f;
    CHECK(near(sin(x), 0.479425538604203), "sin %g", sin(x));
    CHECK(near(cos(x), 0.8775825618903728), "cos");
    CHECK(near(tan(x), 0.5463024898437905), "tan");
    CHECK(near(asin(x), 0.5235987755982989), "asin");
    CHECK(near(acos(x), 1.0471975511965979), "acos");
    CHECK(near(atan(x), 0.4636476090008061), "atan");
    CHECK(near(atan2(x, y), 0.24497866312686414), "atan2");
    CHECK(near(sinh(x), 0.5210953054937474) && near(cosh(x), 1.1276259652063807) && near(tanh(x), 0.46211715726000974), "hyp");
    CHECK(near(exp(x), 1.6487212707001282) && near(log(y), 0.6931471805599453) && near(log10(100.0 * y), 2.3010299956639813), "exp/log");
    CHECK(near(pow(y, 10.5), 1448.1546878700492), "pow");
    CHECK(near(fmod(7.5, y), 1.5), "fmod");
    CHECK(ceil(neg) == -3.0 && floor(neg) == -4.0 && round(neg) == -4.0 && rint(2.5) == 2.0, "rounding");
    CHECK(nearf(sinf(xf), 0.4794255f) && nearf(cosf(xf), 0.87758255f) && nearf(tanf(xf), 0.5463025f), "sinf/cosf/tanf");
    CHECK(nearf(asinf(xf), 0.52359878f) && nearf(acosf(xf), 1.0471976f) && nearf(atanf(xf), 0.4636476f), "asinf..");
    CHECK(nearf(atan2f(xf, yf), 0.24497866f) && nearf(powf(yf, 3.5f), 11.313708f) && nearf(fmodf(7.5f, yf), 1.5f), "atan2f/powf/fmodf");
    CHECK(nearf(expf(xf), 1.6487212f) && nearf(exp2f(3.5f), 11.313708f) && nearf(logf(yf), 0.6931472f) && nearf(log10f(1000.f), 3.f), "expf..");
    CHECK(ceilf(-1.5f) == -1.f && floorf(-1.5f) == -2.f && roundf(2.5f) == 3.f && rintf(2.5f) == 2.f, "float rounding");
    double ip;
    double fr = modf(neg, &ip);
    CHECK(near(fr, -0.7) && ip == -3.0, "modf %g %g", fr, ip);
    int e;
    double m = frexp(48.0, &e);
    CHECK(m == 0.75 && e == 6, "frexp %g %d", m, e);
    float mf = frexpf(48.0f, &e);
    CHECK(mf == 0.75f && e == 6, "frexpf");
    CHECK(ldexp(0.75, 6) == 48.0, "ldexp");
    volatile double nanv = NAN, infv = INFINITY;
    volatile float nanf_ = NAN, inff = INFINITY;
    CHECK(isnan(nanv) && !isnan(x) && isnan(nanf_) && !isnan(xf), "isnan");
    CHECK(isinf(infv) && !isinf(x) && isinf(inff) && !isinf(xf), "isinf");
    CHECK(isfinite(xf) && !isfinite(inff), "isfinite");
    // compiler-rt
    volatile int64_t a = -1234567890123LL, b = 1000;
    volatile uint64_t ua = 0xF234567890ABCDEFULL, ub = 12345;
    CHECK(a / b == -1234567890LL, "divdi3");
    CHECK(a % b == -123LL, "moddi3");
    CHECK(ua / ub == 0xF234567890ABCDEFULL / 12345, "udivdi3");
    CHECK(ua % ub == 0xF234567890ABCDEFULL % 12345, "umoddi3");
    CHECK((uint64_t)big == 10000000000000000000ULL, "fixunsdfdi %llu", (unsigned long long)(uint64_t)big);
    volatile float bigf = 1e19f;
    CHECK((uint64_t)bigf == 9999999980506447872ULL, "fixunssfdi %llu", (unsigned long long)(uint64_t)bigf);
    CHECK(__isnand(nanv) == 1 && __isnand(x) == 0 && __isnanf(nanf_) == 1 && __isnanf(xf) == 0, "__isnan*");
    CHECK(__isinfd(-infv) == 1 && __isinfd(x) == 0 && __isinff(inff) == 1 && __isinff(xf) == 0, "__isinf*");
    CHECK(__isfinitef(xf) == 1 && __isfinitef(inff) == 0 && __isfinitef(nanf_) == 0, "__isfinitef");
    CHECK(__fixunsdfdi(big) == 10000000000000000000ULL && __fixunsdfdi(-5.0) == 0 && __fixunsdfdi(1e30) == UINT64_MAX, "__fixunsdfdi");
    CHECK(__fixunssfdi(bigf) == 9999999980506447872ULL && __fixunssfdi(-1.f) == 0 && __fixunssfdi(3.9f) == 3, "__fixunssfdi");
    CHECK(__divdi3(a, b) == -1234567890LL && __moddi3(a, b) == -123 && __divdi3(INT64_MIN, -1) == INT64_MIN, "__divdi3/__moddi3");
    CHECK(__udivdi3(ua, ub) == 0xF234567890ABCDEFULL / 12345 && __umoddi3(ua, ub) == 0xF234567890ABCDEFULL % 12345, "__udivdi3/__umoddi3");
    DONE();
}
