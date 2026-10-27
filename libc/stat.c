#include <abi/file.h>
#include <directory.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <pyxis/file.h>
#include <pyxis/working_path.h>
#include <resource.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "descriptor.h"
#include "errors.h"
#include "working_path_internal.h"

enum call_status libc_stat_result(const struct file_info_reply *info, mode_t mode,
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
  if (info->valid & FILE_INFO_LINK_COUNT_VALID) {
    value.st_valid |= STAT_NLINK_VALID;
    value.st_nlink = info->link_count;
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
  return status == CALL_OK ? libc_stat_result(&info, S_IFREG, result) : status;
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
  if (status == CALL_WRONG_TYPE || status == CALL_DENIED) {
    /* A directory needs no rights of its own to be identified. */
    enum call_status file_status = status;
    handle_t directory;
    status = directory_open_path(path, 0, &directory);
    if (status == CALL_OK) {
      struct file_info_reply info;
      status = directory_info(directory, &info);
      handle_close(directory);
      if (status == CALL_OK) {
        status = libc_stat_result(&info, S_IFDIR, result);
      }
    } else if (file_status == CALL_DENIED) {
      status = file_status;
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

static int path_metadata(const char *path, struct stat *metadata, uint64_t *rights,
    bool native_only)
{
  if (!path || !*path || !metadata || !rights) {
    errno = EINVAL;
    return -1;
  }
  struct pyxis_working_snapshot working = {0};
  enum call_status status = libc_path_root_prefix(path) ? CALL_OK :
      pyxis_working_snapshot_init(&working, NULL);
  struct resource_result opened = {0};
  if (status == CALL_OK) {
    status = native_only ?
        resource_open_native_metadata_path(&working.context, path, &opened) :
        resource_open_metadata_path(&working.context, path, &opened);
  }
  pyxis_working_snapshot_close(&working);
  if (status == CALL_OK) {
    status = resource_outcome_status(opened.info.description.outcome);
  }
  struct stat value;
  if (status == CALL_OK) {
    if (opened.info.protocol == PROTOCOL_FILE) {
      status = libc_file_stat(opened.resource, &value);
    } else if (opened.info.protocol == PROTOCOL_DIRECTORY &&
        opened.info.kind == HANDLE_KIND_NATIVE && !opened.info.transport) {
      struct file_info_reply info;
      status = directory_info(opened.resource, &info);
      if (status == CALL_OK) {
        status = libc_stat_result(&info, S_IFDIR, &value);
      }
    } else {
      status = CALL_BAD_OPERATION;
    }
  }
  uint64_t actual_rights = opened.info.rights;
  enum call_status closed = resource_result_close(&opened);
  if (status == CALL_OK) {
    status = closed;
  }
  if (status != CALL_OK) {
    errno = libc_path_errno(status, path, NULL);
    return -1;
  }
  *metadata = value;
  *rights = actual_rights;
  return 0;
}

int pyxis_path_metadata(const char *path, struct stat *metadata, uint64_t *rights)
{
  return path_metadata(path, metadata, rights, false);
}

int pyxis_path_metadata_native(const char *path, struct stat *metadata, uint64_t *rights)
{
  return path_metadata(path, metadata, rights, true);
}
