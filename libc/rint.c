#include <math.h>

/* fesetround keeps the x87 and SSE rounding controls aligned. */
double rint(double value)
{
  double rounded;
  __asm__ volatile("fldl %1; frndint; fstpl %0"
    : "=m"(rounded) : "m"(value) : "st");
  return rounded;
}
