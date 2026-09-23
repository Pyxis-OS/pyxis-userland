#include <startup.h>
#include <stdlib.h>
#include "runtime.h"

extern int main(int argc, char **argv);

[[noreturn]] void libc_enter(const struct startup_info *info)
{
  if (!startup_init(info)) {
    _Exit(EXIT_FAILURE);
  }
  malloc_init();
  stdio_init();
  exit(main((int)info->argc, (char **)(uintptr_t)info->argv));
}
