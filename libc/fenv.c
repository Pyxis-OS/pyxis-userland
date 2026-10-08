#include <fenv.h>
#include <stdint.h>

/* x87 control word: exception masks in bits 0-5, rounding control in 10-11. */
#define X87_EXCEPTION_MASKS UINT16_C(0x003f)
#define X87_ROUNDING_MASK UINT16_C(0x0c00)
#define X87_DEFAULT_CONTROL UINT16_C(0x037f)

/* MXCSR: flags in bits 0-5, masks in 7-12 and rounding control in 13-14,
 * the x87 rounding encoding shifted left by three. */
#define MXCSR_FLAGS UINT32_C(0x003f)
#define MXCSR_EXCEPTION_MASKS UINT32_C(0x1f80)
#define MXCSR_ROUNDING_SHIFT 3
#define MXCSR_ROUNDING_MASK (UINT32_C(0x0c00) << MXCSR_ROUNDING_SHIFT)
#define MXCSR_DEFAULT UINT32_C(0x1f80)

const fenv_t __fe_dfl_env = {
  .x87_control = X87_DEFAULT_CONTROL,
  .mxcsr = MXCSR_DEFAULT,
};

static uint16_t x87_status(void)
{
  uint16_t status;
  __asm__ volatile("fnstsw %0" : "=m"(status));
  return status;
}

static uint16_t x87_control(void)
{
  uint16_t control;
  __asm__ volatile("fnstcw %0" : "=m"(control));
  return control;
}

static void set_x87_control(uint16_t control)
{
  __asm__ volatile("fldcw %0" : : "m"(control));
}

static void clear_x87_flags(void)
{
  __asm__ volatile("fnclex");
}

static uint32_t mxcsr(void)
{
  uint32_t value;
  __asm__ volatile("stmxcsr %0" : "=m"(value));
  return value;
}

static void set_mxcsr(uint32_t value)
{
  __asm__ volatile("ldmxcsr %0" : : "m"(value));
}

/* Move raised x87 flags into MXCSR so that one register holds them all. */
static uint32_t gather_flags(void)
{
  uint32_t value = mxcsr();
  uint16_t raised = x87_status() & FE_ALL_EXCEPT;
  if (raised) {
    clear_x87_flags();
    value |= raised;
    set_mxcsr(value);
  }
  return value;
}

int feclearexcept(int excepts)
{
  set_mxcsr(gather_flags() & ~(uint32_t)(excepts & FE_ALL_EXCEPT));
  return 0;
}

int fegetexceptflag(fexcept_t *flags, int excepts)
{
  *flags = (fexcept_t)fetestexcept(excepts);
  return 0;
}

int feraiseexcept(int excepts)
{
  set_mxcsr(mxcsr() | (uint32_t)(excepts & FE_ALL_EXCEPT));
  return 0;
}

int fesetexceptflag(const fexcept_t *flags, int excepts)
{
  uint32_t selected = (uint32_t)(excepts & FE_ALL_EXCEPT);
  set_mxcsr((gather_flags() & ~selected) | (*flags & selected));
  return 0;
}

int fetestexcept(int excepts)
{
  return (int)((mxcsr() | x87_status()) & (uint32_t)(excepts & FE_ALL_EXCEPT));
}

int fegetround(void)
{
  return (int)((mxcsr() & MXCSR_ROUNDING_MASK) >> MXCSR_ROUNDING_SHIFT);
}

int fesetround(int round)
{
  if (round != FE_TONEAREST && round != FE_DOWNWARD && round != FE_UPWARD &&
      round != FE_TOWARDZERO) {
    return -1;
  }
  set_x87_control((x87_control() & ~X87_ROUNDING_MASK) | (uint16_t)round);
  set_mxcsr((mxcsr() & ~MXCSR_ROUNDING_MASK) | ((uint32_t)round << MXCSR_ROUNDING_SHIFT));
  return 0;
}

int fegetenv(fenv_t *environment)
{
  environment->x87_control = x87_control();
  environment->mxcsr = gather_flags();
  return 0;
}

int feholdexcept(fenv_t *environment)
{
  fegetenv(environment);
  set_x87_control(environment->x87_control | X87_EXCEPTION_MASKS);
  set_mxcsr((environment->mxcsr & ~MXCSR_FLAGS) | MXCSR_EXCEPTION_MASKS);
  return 0;
}

int fesetenv(const fenv_t *environment)
{
  clear_x87_flags();
  set_x87_control(environment->x87_control);
  set_mxcsr(environment->mxcsr);
  return 0;
}

int feupdateenv(const fenv_t *environment)
{
  int raised = fetestexcept(FE_ALL_EXCEPT);
  fesetenv(environment);
  feraiseexcept(raised);
  return 0;
}
