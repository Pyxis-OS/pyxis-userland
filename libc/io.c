#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pyxis/file.h>
#include <stdarg.h>
#include <unistd.h>
#include "descriptor.h"

static int open_mode(int flags, struct descriptor_mode *mode)
{
  int access = flags & (O_WRONLY | O_RDWR);
  if ((flags & ~(O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_EXCL | O_APPEND | O_NOFOLLOW)) ||
      access == (O_WRONLY | O_RDWR) ||
      (access == O_RDONLY && (flags & (O_CREAT | O_TRUNC))) ||
      ((flags & O_EXCL) && !(flags & O_CREAT))) {
    errno = EINVAL;
    return -1;
  }
  *mode = (struct descriptor_mode){
    .readable = access != O_WRONLY, .writable = access != O_RDONLY,
    .create = flags & O_CREAT, .truncate = flags & O_TRUNC,
    .exclusive = flags & O_EXCL, .append = flags & O_APPEND,
  };
  return 0;
}

static int validate_creation_mode(int flags, mode_t mode)
{
  /* Native creation has no permission-mode argument. Accept only the
   * agreed default; never silently discard a restrictive mode request. */
  if ((flags & O_CREAT) && mode != 0666) {
    errno = ENOTSUP;
    return -1;
  }
  return 0;
}

int open(const char *path, int flags, ...)
{
  struct descriptor_mode mode;
  if (open_mode(flags, &mode) < 0) {
    return -1;
  }
  if (mode.create) {
    va_list args;
    va_start(args, flags);
    mode_t creation_mode = va_arg(args, mode_t);
    va_end(args);
    if (validate_creation_mode(flags, creation_mode) < 0) {
      return -1;
    }
  }
  return descriptor_open(path, &mode, NULL);
}

int pyxis_file_open(const char *path, int flags, mode_t creation)
{
  struct descriptor_mode mode;
  if (open_mode(flags, &mode) < 0 || validate_creation_mode(flags, creation) < 0) {
    return -1;
  }
  return mode.writable ? descriptor_open_native_file(path, &mode) :
      descriptor_open(path, &mode, NULL);
}

static bool transfer_ready(struct descriptor_object *object, size_t count, bool writing)
{
  if (!descriptor_object_ready(object, writing)) {
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
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!transfer_ready(object, count, false)) {
    return -1;
  }
  size_t transferred;
  if (descriptor_object_read_complete(object, buffer, count, &transferred) < 0) {
    return -1;
  }
  return (ssize_t)transferred;
}

ssize_t write(int descriptor, const void *buffer, size_t count)
{
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!transfer_ready(object, count, true)) {
    return -1;
  }
  size_t transferred;
  if (descriptor_object_write(object, buffer, count, &transferred) < 0) {
    return -1;
  }
  return (ssize_t)transferred;
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!transfer_ready(object, count, false)) {
    return -1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  size_t transferred;
  if (descriptor_object_pread(object, buffer, count, (uint64_t)offset, &transferred) < 0) {
    return -1;
  }
  return (ssize_t)transferred;
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!transfer_ready(object, count, true)) {
    return -1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  size_t transferred;
  if (descriptor_object_pwrite(object, buffer, count, (uint64_t)offset, &transferred) < 0) {
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
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!descriptor_object_ready(object, true)) {
    return -1;
  }
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  return descriptor_object_resize(object, (uint64_t)length);
}

int truncate(const char *path, off_t length)
{
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  int descriptor = open(path, O_WRONLY);
  if (descriptor < 0) {
    return -1;
  }
  int result = ftruncate(descriptor, length);
  int saved = errno;
  close(descriptor);
  errno = saved;
  return result;
}

off_t lseek(int descriptor, off_t offset, int origin)
{
  return descriptor_seek_position(descriptor, offset, origin);
}

int fsync(int descriptor)
{
  struct descriptor_object *object __attribute__((cleanup(descriptor_scope_end))) =
      descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  if (!descriptor_object_ready(object, true)) {
    return -1;
  }
  return descriptor_object_sync(object);
}

int isatty(int descriptor)
{
  return descriptor_terminal(descriptor);
}
