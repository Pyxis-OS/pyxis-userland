#include <abi/file.h>
#include <directory.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <sys/stat.h>
#include "descriptor.h"
#include "errors.h"

static enum call_status stat_result(const struct file_info_reply *info, mode_t mode,
    struct stat *result)
{
  if (mode == S_IFREG && info->size > LONG_MAX) {
    return CALL_LIMIT;
  }
  struct stat value = {.st_mode = mode, .st_size = mode == S_IFREG ? (off_t)info->size : 0};
  if (info->valid & FILE_INFO_DOMAIN_VALID) {
    value.st_valid |= STAT_DEV_VALID;
    value.st_dev = info->domain;
  }
  if (info->valid & FILE_INFO_OBJECT_VALID) {
    value.st_valid |= STAT_INO_VALID;
    value.st_ino = info->object;
  }
  if (info->valid & FILE_INFO_MTIME_VALID) {
    value.st_valid |= STAT_MTIME_VALID;
    value.st_mtim = (struct timespec){info->modified_seconds, (long)info->modified_nanoseconds};
  }
  *result = value;
  return CALL_OK;
}

enum call_status libc_file_stat(handle_t file, struct stat *result)
{
  struct file_info_reply info;
  enum call_status status = file_info(file, &info);
  if (status == CALL_OK && !(info.valid & FILE_INFO_SIZE_VALID)) {
    status = file_size(file, &info.size);
    if (status == CALL_OK) {
      info.valid |= FILE_INFO_SIZE_VALID;
    }
  }
  return status == CALL_OK ? stat_result(&info, S_IFREG, result) : status;
}

/* INFO accepts READ or WRITE, so either file grant can report metadata. */
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
  status = libc_file_stat(file, result);
  handle_close(file);
  return status;
}

int stat(const char *restrict path, struct stat *restrict result)
{
  enum call_status status = file_metadata(path, result);
  if (status == CALL_WRONG_TYPE) {
    /* A directory needs no rights of its own to be identified. */
    handle_t directory;
    status = directory_open_path(path, 0, &directory);
    if (status == CALL_OK) {
      struct file_info_reply info;
      status = directory_info(directory, &info);
      handle_close(directory);
      if (status == CALL_OK) {
        status = stat_result(&info, S_IFDIR, result);
      }
    }
  }
  if (status != CALL_OK) {
    errno = libc_path_errno(status, path, NULL);
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
