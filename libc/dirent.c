#include <abi/directory.h>
#include <abi/file.h>
#include <directory.h>
#include <dirent.h>
#include <errno.h>
#include <handle.h>
#include <pyxis/directory.h>
#include <string.h>
#include <stdlib.h>
#include "descriptor.h"
#include "errors.h"

/* Grows to the longest name seen; native names have no fixed maximum. */
#define INITIAL_NAME_CAPACITY 64

struct libc_directory {
  handle_t handle;
  struct directory_cursor cursor;
  struct dirent *entry; /* Owned; returned by readdir. */
  size_t name_capacity;
};

static unsigned char entry_type(uint64_t kind)
{
  switch (kind) {
  case DIRECTORY_KIND_FILE: return DT_REG;
  case DIRECTORY_KIND_DIRECTORY: return DT_DIR;
  case DIRECTORY_KIND_SYMLINK: return DT_LNK;
  default: return DT_UNKNOWN;
  }
}

static DIR *open_directory(const char *path, bool native_only)
{
  DIR *directory = malloc(sizeof(*directory));
  struct dirent *entry = malloc(sizeof(*entry) + INITIAL_NAME_CAPACITY);
  if (!directory || !entry) {
    free(entry);
    free(directory);
    errno = ENOMEM;
    return NULL;
  }
  handle_t handle;
  enum call_status status = native_only ? libc_directory_open_native(path, &handle) :
      directory_open_path(path, DIRECTORY_RIGHT_ENUMERATE, &handle);
  if (status != CALL_OK) {
    free(entry);
    free(directory);
    errno = native_only && status == CALL_WRONG_TYPE ? ENOTDIR :
        libc_path_errno(status, path, NULL);
    return NULL;
  }
  *directory = (struct libc_directory){
    .handle = handle, .entry = entry, .name_capacity = INITIAL_NAME_CAPACITY,
  };
  return directory;
}

DIR *opendir(const char *path)
{
  return open_directory(path, false);
}

DIR *pyxis_directory_open(const char *path)
{
  return open_directory(path, true);
}

/* The DIR owner must stay live. These calls access only its immutable handle. */
static enum call_status entry_parent(DIR *directory, const char *component,
    struct handle_info *info)
{
  if (!directory || !component || !*component || strchr(component, '/') ||
      !strcmp(component, ".") || !strcmp(component, "..")) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = handle_query(directory->handle, info);
  if (status == CALL_OK && (info->protocol != PROTOCOL_DIRECTORY ||
      info->kind != HANDLE_KIND_NATIVE || info->transport)) {
    status = CALL_BAD_OPERATION;
  }
  return status;
}

int pyxis_directory_entry_metadata(DIR *directory, const char *component,
    struct stat *metadata, uint64_t *rights)
{
  if (!metadata || !rights) {
    errno = EINVAL;
    return -1;
  }
  struct handle_info parent;
  enum call_status status = entry_parent(directory, component, &parent);
  struct directory_typed_lookup_reply child = {0};
  if (status == CALL_OK) {
    uint64_t file_rights = ((parent.rights & DIRECTORY_RIGHT_READ_FILES) ? FILE_RIGHT_READ : 0) |
        ((parent.rights & DIRECTORY_RIGHT_WRITE_FILES) ? FILE_RIGHT_WRITE : 0);
    status = directory_lookup_typed(directory->handle, component, file_rights,
        parent.rights & DIRECTORY_RIGHTS, &child);
  }
  struct stat value;
  struct handle_info actual;
  if (status == CALL_OK) {
    status = handle_query(child.handle, &actual);
  }
  if (status == CALL_OK) {
    if (child.protocol == PROTOCOL_FILE) {
      status = libc_file_stat(child.handle, &value);
    } else {
      struct file_info_reply info;
      status = directory_info(child.handle, &info);
      if (status == CALL_OK) {
        status = libc_stat_result(&info, S_IFDIR, &value);
      }
    }
  }
  if (child.handle != HANDLE_INVALID && handle_close(child.handle) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    errno = libc_path_errno(status, component, NULL);
    return -1;
  }
  *metadata = value;
  *rights = actual.rights;
  return 0;
}

int pyxis_directory_entry_type(DIR *directory, const char *component, unsigned char *type)
{
  if (!type) {
    errno = EINVAL;
    return -1;
  }
  struct handle_info parent;
  enum call_status status = entry_parent(directory, component, &parent);
  struct directory_typed_lookup_reply child = {0};
  if (status == CALL_OK) {
    status = directory_lookup_typed(directory->handle, component, 0, 0, &child);
  }
  unsigned char value = child.protocol == PROTOCOL_DIRECTORY ? DT_DIR : DT_REG;
  if (child.handle != HANDLE_INVALID && handle_close(child.handle) != 0 && status == CALL_OK) {
    status = CALL_IO;
  }
  if (status != CALL_OK) {
    errno = libc_path_errno(status, component, NULL);
    return -1;
  }
  *type = value;
  return 0;
}

struct dirent *readdir(DIR *directory)
{
  for (;;) {
    struct directory_enumerate_reply reply;
    enum call_status status = directory_enumerate(directory->handle, &directory->cursor,
        directory->entry->d_name, directory->name_capacity, &reply);
    if (status != CALL_OK) {
      errno = libc_call_errno(status);
      return NULL;
    }
    if (reply.outcome == DIRECTORY_END) {
      return NULL;
    }
    if (reply.outcome == DIRECTORY_CHANGED) {
      errno = EAGAIN;
      return NULL;
    }
    if (reply.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      /* The cursor is unchanged, so retry the same entry with room for it. */
      struct dirent *larger = realloc(directory->entry, sizeof(*larger) + reply.name_size);
      if (!larger) {
        errno = ENOMEM;
        return NULL;
      }
      directory->entry = larger;
      directory->name_capacity = reply.name_size;
      continue;
    }
    if (reply.outcome != DIRECTORY_ENTRY) {
      errno = EIO;
      return NULL;
    }
    directory->cursor = reply.cursor;
    directory->entry->d_type = entry_type(reply.kind);
    return directory->entry;
  }
}

int closedir(DIR *directory)
{
  int result = handle_close(directory->handle);
  free(directory->entry);
  free(directory);
  if (result != 0) {
    errno = EBADF;
    return -1;
  }
  return 0;
}
