#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include "descriptor.h"

int open(const char *path, int flags, ...)
{
  if (flags != O_RDONLY) {
    errno = EINVAL;
    return -1;
  }
  const struct descriptor_mode mode = {.readable = true};
  return descriptor_open(path, &mode, NULL);
}

static bool transfer_ready(int descriptor, size_t count, bool writing)
{
  if (!descriptor_ready(descriptor, writing)) {
    return false;
  }
  if (count > (size_t)SSIZE_MAX) {
    errno = EINVAL;
    return false;
  }
  return true;
}

ssize_t read(int descriptor, void *buffer, size_t count)
{
  if (!transfer_ready(descriptor, count, false)) {
    return -1;
  }
  size_t transferred;
  if (descriptor_read(descriptor, buffer, count, &transferred) < 0) {
    return -1;
  }
  return (ssize_t)transferred;
}

ssize_t write(int descriptor, const void *buffer, size_t count)
{
  if (!transfer_ready(descriptor, count, true)) {
    return -1;
  }
  size_t transferred;
  if (descriptor_write(descriptor, buffer, count, &transferred) < 0) {
    return -1;
  }
  return (ssize_t)transferred;
}

int close(int descriptor)
{
  return descriptor_close(descriptor);
}
