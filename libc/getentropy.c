#include <clock.h>
#include <errno.h>
#include <random.h>
#include <startup.h>
#include <environment.h>
#include <handle.h>
#include "thread_private.h"
#include "thread_platform.h"
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
  handle_t clock = libc_thread_clock();
  handle_t random;
  enum call_status captured = environment_capability(startup_environment_handle(), "random", &random);
  if (captured != CALL_OK) {
    errno = captured == CALL_NOT_FOUND ? ENODEV : libc_call_errno(captured);
    return -1;
  }
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    handle_close(random);
    errno = libc_call_errno(status);
    return -1;
  }
  if (now > UINT64_MAX - RANDOM_MAX_WAIT_NS) {
    handle_close(random);
    errno = EOVERFLOW;
    return -1;
  }
  status = random_read(random, buffer, length, now + RANDOM_MAX_WAIT_NS);
  int released = handle_close(random);
  if (status != CALL_OK || released) {
    errno = status != CALL_OK ? libc_call_errno(status) : EIO;
    return -1;
  }
  return 0;
}
