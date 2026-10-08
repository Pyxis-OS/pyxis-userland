#include <stdlib.h>

int abs(int value)
{
  return value < 0 ? -value : value;
}

long labs(long value)
{
  return value < 0 ? -value : value;
}

long long llabs(long long value)
{
  return value < 0 ? -value : value;
}

div_t div(int numerator, int denominator)
{
  return (div_t){numerator / denominator, numerator % denominator};
}

ldiv_t ldiv(long numerator, long denominator)
{
  return (ldiv_t){numerator / denominator, numerator % denominator};
}

lldiv_t lldiv(long long numerator, long long denominator)
{
  return (lldiv_t){numerator / denominator, numerator % denominator};
}
