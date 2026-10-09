#include <abi/file.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <sys/stat.h>
#include "descriptor.h"
#include "errors.h"

/* FILE_SIZE accepts READ or WRITE, so a file opened with either can be sized. */
static enum call_status file_metadata(const char *path, struct stat *result)
{
  handle_t file;
  enum call_status status = file_open_path(path, FILE_RIGHT_READ, false, &file);
  if (status == CALL_DENIED) {
    status = file_open_path(path, FILE_RIGHT_WRITE, false, &file);
  }
  if (status != CALL_OK) {
    return status;
  }
  uint64_t size;
  status = file_size(file, &size);
  handle_close(file);
  if (status != CALL_OK) {
    return status;
  }
  if (size > LONG_MAX) {
    return CALL_LIMIT;
  }
  *result = (struct stat){.st_mode = S_IFREG, .st_size = (off_t)size};
  return CALL_OK;
}

int stat(const char *restrict path, struct stat *restrict result)
{
  enum call_status status = file_metadata(path, result);
  if (status == CALL_WRONG_TYPE) {
    /* A directory needs no rights of its own to be identified. */
    handle_t directory;
    status = directory_open_path(path, 0, &directory);
    if (status == CALL_OK) {
      handle_close(directory);
      *result = (struct stat){.st_mode = S_IFDIR};
    }
  }
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}

int lstat(const char *restrict path, struct stat *restrict result)
{
  return stat(path, result);
}

int fstat(int descriptor, struct stat *result)
{
  return descriptor_stat(descriptor, result);
}
