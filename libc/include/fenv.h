#ifndef LIBC_FENV_H
#define LIBC_FENV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exception flags and rounding modes share the x87 and SSE bit positions.
 * The x86 denormal-operand flag is not a C exception and is left alone. */
#define FE_INVALID 0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST 0x000
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xc00

typedef unsigned short fexcept_t;

/* float/double use SSE and long double uses x87, so the environment holds
 * both control registers. Raised flags of both units are kept in mxcsr. */
typedef struct {
  unsigned short x87_control;
  unsigned int mxcsr;
} fenv_t;

/* The process start environment: round to nearest, exceptions masked and
 * flags clear. */
extern const fenv_t __fe_dfl_env;
#define FE_DFL_ENV (&__fe_dfl_env)

/* These functions control both units and always succeed, except fesetround
 * with an unknown mode. Exceptions stay masked unless an fenv_t built by the
 * caller unmasks them; an unmasked exception faults the process like any
 * other CPU exception. Code that reads flags or runs under a non-default
 * rounding mode needs #pragma STDC FENV_ACCESS ON so the compiler keeps its
 * operations in order. */
int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flags, int excepts);
/* Sets the flags without trapping, as masked exceptions would. */
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flags, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int round);
int fegetenv(fenv_t *environment);
/* Saves the environment, clears the flags and masks every exception. */
int feholdexcept(fenv_t *environment);
int fesetenv(const fenv_t *environment);
int feupdateenv(const fenv_t *environment);

#ifdef __cplusplus
}
#endif

#endif
