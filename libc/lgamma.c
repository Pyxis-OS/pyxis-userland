#include <math.h>

double __lgamma_r(double x, int *sign);

/* musl's lgamma also stores the sign in the signgam global, which is shared
 * between threads. Pyxis keeps no such state: the sign is discarded. */
double lgamma(double x)
{
  int sign;
  return __lgamma_r(x, &sign);
}
