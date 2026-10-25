#include <math.h>

#pragma STDC FENV_ACCESS ON

float nearbyintf(float value)
{
  /* Float-to-double is exact. An integral result is also exactly representable
   * as float: smaller values round within its integer precision; larger ones
   * were integral already. nearbyint preserves the existing inexact flag. */
  return (float)nearbyint((double)value);
}
