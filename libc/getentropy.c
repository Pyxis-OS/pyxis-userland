#include <clock.h>
#include <errno.h>
#include <random.h>
#include <startup.h>
#include <sys/random.h>
#include "errors.h"

int getentropy(void *buffer, size_t length)
{
  if (length > RANDOM_MAX_BYTES) {
    errno = EIO;
    return -1;
  }
  if (length && !buffer) {
    errno = EFAULT;
    return -1;
  }
  handle_t clock = startup_resource("clock");
  handle_t random = startup_resource("random");
  if (clock == HANDLE_INVALID || random == HANDLE_INVALID) {
    errno = ENODEV;
    return -1;
  }
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  if (now > UINT64_MAX - RANDOM_MAX_WAIT_NS) {
    errno = EOVERFLOW;
    return -1;
  }
  status = random_read(random, buffer, length, now + RANDOM_MAX_WAIT_NS);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}
