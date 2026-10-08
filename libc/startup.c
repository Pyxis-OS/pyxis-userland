#include <startup.h>
#include <stdlib.h>
#include "runtime.h"

extern int main(int argc, char **argv);

/* The SDK linker script defines the bounds. Weak references keep links without
 * it valid; an absent array has equal (null) bounds. */
extern void (*const __init_array_start[])(void) __attribute__((weak));
extern void (*const __init_array_end[])(void) __attribute__((weak));

/* Constructors, including C++ static initializers, may use stdio and the heap. */
static void run_init_array(void)
{
  for (size_t index = 0; index < (size_t)(__init_array_end - __init_array_start); index++) {
    __init_array_start[index]();
  }
}

[[noreturn]] void libc_enter(const struct startup_info *info)
{
  if (!startup_init(info)) {
    _Exit(EXIT_FAILURE);
  }
  stdio_init();
  malloc_init();
  run_init_array();
  exit(main((int)info->argc, (char **)(uintptr_t)info->argv));
}
