#include <stdint.h>
#include <stdlib.h>
#include "thread_sync.h"

/* ISO C's portable example generator: a 32-bit linear congruential state
 * whose upper bits give 15-bit results. */
#define RAND_MULTIPLIER UINT32_C(1103515245)
#define RAND_INCREMENT UINT32_C(12345)

static uint32_t state = 1;
static struct libc_lock random_guard;

int rand(void)
{
  libc_lock_acquire(&random_guard);
  state = state * RAND_MULTIPLIER + RAND_INCREMENT;
  int result = (int)((state >> 16) % (RAND_MAX + 1u));
  libc_lock_release(&random_guard);
  return result;
}

void srand(unsigned seed)
{
  libc_lock_acquire(&random_guard);
  state = seed;
  libc_lock_release(&random_guard);
}
