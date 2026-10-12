#include <errno.h>
#include "thread_private.h"

int *__errno_location(void)
{
  return &libc_thread_record()->errno_value;
}
