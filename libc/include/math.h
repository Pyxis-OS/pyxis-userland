#ifndef LIBC_MATH_H
#define LIBC_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* The Pyxis SDK uses SSE2 evaluation for float/double and x87 long double. */
#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ != 0
#error Unsupported floating-point evaluation model
#endif
typedef float float_t;
typedef double double_t;

/* C23 float.h may already expose these. Use musl's forms for older compilers. */
#undef NAN
#undef INFINITY
#if 100*__GNUC__+__GNUC_MINOR__ >= 303
#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#else
#define NAN (0.0f/0.0f)
#define INFINITY 1e5000f
#endif
#define HUGE_VALF INFINITY
#define HUGE_VAL ((double)INFINITY)
#define HUGE_VALL ((long double)INFINITY)

/* POSIX constants, with musl's values. */
#define M_E             2.7182818284590452354   /* e */
#define M_LOG2E         1.4426950408889634074   /* log_2 e */
#define M_LOG10E        0.43429448190325182765  /* log_10 e */
#define M_LN2           0.69314718055994530942  /* log_e 2 */
#define M_LN10          2.30258509299404568402  /* log_e 10 */
#define M_PI            3.14159265358979323846  /* pi */
#define M_PI_2          1.57079632679489661923  /* pi/2 */
#define M_PI_4          0.78539816339744830962  /* pi/4 */
#define M_1_PI          0.31830988618379067154  /* 1/pi */
#define M_2_PI          0.63661977236758134308  /* 2/pi */
#define M_2_SQRTPI      1.12837916709551257390  /* 2/sqrt(pi) */
#define M_SQRT2         1.41421356237309504880  /* sqrt(2) */
#define M_SQRT1_2       0.70710678118654752440  /* 1/sqrt(2) */

/* fpclassify results. */
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

/* Exceptional integer-exponent results used by the C math contract. */
#define FP_ILOGB0 (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

/* Classification macros accept any real floating type, evaluate the argument
 * once and raise no FP exception. The comparison macros (isgreater and the
 * like) are not provided. */
#define fpclassify(value) \
  __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, value)
#define isfinite(value) __builtin_isfinite(value)
#define isinf(value) __builtin_isinf(value)
#define isnan(value) __builtin_isnan(value)
#define isnormal(value) __builtin_isnormal(value)
#define signbit(value) __builtin_signbit(value)

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT

/* This subset reports math errors through FP exception flags, not errno.
 * The process starts with FP traps masked; see fenv.h. Signaling NaNs are
 * not supported.
 * These functions live in libc; callers do not need a separate -lm. */
double floor(double value);
float floorf(float value);
/* Nearest integer, ties away from zero, independent of the FP rounding mode. */
double round(double value);
float roundf(float value);
/* Same rounding as round; the integer result is unspecified when it does not
 * fit, including infinity and NaN. Fractional inputs may raise inexact. */
long lround(double value);
/* Integral part, rounded toward zero independently of the FP rounding mode. */
double trunc(double value);
double fmod(double value, double divisor);
float fmodf(float value, float divisor);
/* Store the integral part truncated toward zero and return the signed fraction. */
double modf(double value, double *integral);
double pow(double base, double exponent);
float powf(float base, float exponent);
/* Round to an integral value in the current rounding mode (round to nearest,
 * ties to even, by default). The long forms are unspecified when the result
 * doesn't fit, as in C. */
double rint(double value);
float rintf(float value);
long lrint(double value);
long lrintf(float value);
long long llrintf(float value);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double scalbn(double value, int exponent);
long double scalbnl(long double value, int exponent);
long double frexpl(long double value, int *exponent);
long double ldexpl(long double value, int exponent);
long double fmodl(long double value, long double divisor);
double fabs(double value);
float fabsf(float value);
double ceil(double value);
float ceilf(float value);
/* Correctly rounded, using SSE2 sqrtsd and sqrtss. */
double sqrt(double value);
float sqrtf(float value);
/* Radians. Large arguments use full-precision reduction modulo pi/2. */
double sin(double value);
double cos(double value);
double tan(double value);
double asin(double value);
double acos(double value);
double atan(double value);
double atan2(double y, double x);
double sinh(double value);
double cosh(double value);
double tanh(double value);
float sinf(float value);
float cosf(float value);
float tanf(float value);
float asinf(float value);
float acosf(float value);
float atanf(float value);
float atan2f(float y, float x);
/* Natural and base-10 logarithms. Zero gives -inf with divide-by-zero, negative
 * values NaN with invalid. */
double log(double value);
double log10(double value);
double log2(double value);
float log2f(float value);
/* Binary exponent, including subnormals. logb(0) gives -inf with divide-by-zero;
 * ilogb(0), ilogb(NaN) and ilogb(infinity) raise invalid and return FP_ILOGB0,
 * FP_ILOGBNAN and INT_MAX respectively. */
double logb(double value);
int ilogb(double value);
/* log(1 + value), accurate near zero. -1 gives -inf with divide-by-zero;
 * values below -1 give NaN with invalid. */
double log1p(double value);
double exp(double value);
double exp2(double value);
/* exp(value) - 1, accurate near zero. */
double expm1(double value);
float logf(float value);
float log10f(float value);
float expf(float value);
double erf(double value);
double erfc(double value);
long double fabsl(long double value);
long double copysignl(long double magnitude, long double sign);

/* The remaining double functions of ISO C math.h, from musl 1.2.5. Only the
 * float and long double forms declared in this header are provided. The
 * lround/llround/lrint families beyond those above, the exp/log/trig forms not
 * listed here, the comparison macros and the non-ISO functions (j0, y0, gamma,
 * drem, exp10 and similar) are absent. */
double copysign(double magnitude, double sign);
double fdim(double x, double y);
/* Correctly rounded single-rounding x*y+z. */
double fma(double x, double y, double z);
/* A NaN argument is ignored in favor of the other. Signed zeros compare
 * unequal: fmax(-0.0, 0.0) is +0.0 and fmin is -0.0. */
double fmax(double x, double y);
double fmin(double x, double y);
float fmaxf(float x, float y);
float fminf(float x, float y);
double hypot(double x, double y);
float hypotf(float x, float y);
double cbrt(double value);
double acosh(double value);
double asinh(double value);
double atanh(double value);
/* Like rint but never raises inexact. */
double nearbyint(double value);
double nextafter(double from, double toward);
double nexttoward(double from, long double toward);
double remainder(double x, double y);
/* Stores the low bits (at least three) of the integer quotient with the sign
 * of x/y. */
double remquo(double x, double y, int *quotient);
double scalbln(double value, long exponent);
/* ln |gamma(x)|. The sign of gamma(x) is not reported, and there is no
 * signgam or lgamma_r. */
double lgamma(double value);
double tgamma(double value);

#ifdef __cplusplus
}
#endif

#endif
