#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <unistd.h>
#include "descriptor.h"

int open(const char *path, int flags, ...)
{
  if ((flags & ~(O_WRONLY | O_CREAT | O_TRUNC)) ||
      (!(flags & O_WRONLY) && (flags & (O_CREAT | O_TRUNC)))) {
    errno = EINVAL;
    return -1;
  }
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode_t creation_mode = va_arg(args, mode_t);
    va_end(args);
    /* Native creation has no permission-mode argument. Accept only the
     * agreed default; never silently discard a restrictive mode request. */
    if (creation_mode != 0666) {
      errno = ENOTSUP;
      return -1;
    }
  }
  const struct descriptor_mode mode = {
    .readable = !(flags & O_WRONLY), .writable = flags & O_WRONLY,
    .create = flags & O_CREAT, .truncate = flags & O_TRUNC,
  };
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

int ftruncate(int descriptor, off_t length)
{
  if (!descriptor_ready(descriptor, true)) {
    return -1;
  }
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  return descriptor_resize(descriptor, (uint64_t)length);
}
