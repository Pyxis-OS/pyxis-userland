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

/* fpclassify results. libc does not provide the C classification macros yet;
 * C++ code gets std::fpclassify and the rest from libc++'s <cmath>. */
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4

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
double fmod(double value, double divisor);
float fmodf(float value, float divisor);
/* Store the integral part truncated toward zero and return the signed fraction. */
double modf(double value, double *integral);
double pow(double base, double exponent);
float powf(float base, float exponent);
/* Round to an integral value in the current rounding mode (round to nearest,
 * ties to even, by default). The long forms are unspecified when the result
 * doesn't fit, as in C. */
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
float acosf(float value);
float atanf(float value);
float atan2f(float y, float x);
/* Natural and base-10 logarithms. Zero gives -inf with divide-by-zero, negative
 * values NaN with invalid. */
double log(double value);
double log10(double value);
double exp(double value);
/* exp(value) - 1, accurate near zero. */
double expm1(double value);
float logf(float value);
float log10f(float value);
float expf(float value);
long double fabsl(long double value);
long double copysignl(long double magnitude, long double sign);

#ifdef __cplusplus
}
#endif

#endif
