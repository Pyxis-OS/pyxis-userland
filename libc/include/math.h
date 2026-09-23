#ifndef LIBC_MATH_H
#define LIBC_MATH_H

/* The Pyxis SDK uses SSE2 evaluation for float/double and x87 long double. */
#if defined(__FLT_EVAL_METHOD__) && __FLT_EVAL_METHOD__ != 0
#error Unsupported floating-point evaluation model
#endif
typedef float float_t;
typedef double double_t;

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT

/* This subset reports math errors through FP exception flags, not errno.
 * The process starts with FP traps masked. There is no fenv API yet.
 * These functions live in libc; callers do not need a separate -lm. */
double scalbn(double value, int exponent);
long double scalbnl(long double value, int exponent);
long double ldexpl(long double value, int exponent);
long double fmodl(long double value, long double divisor);
long double fabsl(long double value);
long double copysignl(long double magnitude, long double sign);

#endif
