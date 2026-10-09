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
/* Nearest integer, ties away from zero, independent of the FP rounding mode. */
double round(double value);
float roundf(float value);
double fmod(double value, double divisor);
/* Store the integral part truncated toward zero and return the signed fraction. */
double modf(double value, double *integral);
double pow(double base, double exponent);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double scalbn(double value, int exponent);
long double scalbnl(long double value, int exponent);
long double frexpl(long double value, int *exponent);
long double ldexpl(long double value, int exponent);
long double fmodl(long double value, long double divisor);
double fabs(double value);
double ceil(double value);
float ceilf(float value);
/* Correctly rounded, using SSE2 sqrtsd and sqrtss. */
double sqrt(double value);
float sqrtf(float value);
/* Radians. Large arguments use full-precision reduction modulo pi/2. */
double sin(double value);
double cos(double value);
double tan(double value);
double atan(double value);
double atan2(double y, double x);
/* Natural and base-10 logarithms. Zero gives -inf with divide-by-zero, negative
 * values NaN with invalid. */
double log(double value);
double log10(double value);
long double fabsl(long double value);
long double copysignl(long double magnitude, long double sign);

#ifdef __cplusplus
}
#endif

#endif
