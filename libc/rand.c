#include <stdint.h>
#include <stdlib.h>

/* ISO C's portable example generator: a 32-bit linear congruential state
 * whose upper bits give 15-bit results. */
#define RAND_MULTIPLIER UINT32_C(1103515245)
#define RAND_INCREMENT UINT32_C(12345)

static uint32_t state = 1;

int rand(void)
{
  state = state * RAND_MULTIPLIER + RAND_INCREMENT;
  return (int)((state >> 16) % (RAND_MAX + 1u));
}

void srand(unsigned seed)
{
  state = seed;
}
