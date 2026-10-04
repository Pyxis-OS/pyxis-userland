#include <abi/directory.h>
#include <directory.h>
#include <dirent.h>
#include <errno.h>
#include <handle.h>
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

DIR *opendir(const char *path)
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
  enum call_status status = directory_open_path(path, DIRECTORY_RIGHT_ENUMERATE, &handle);
  if (status != CALL_OK) {
    free(entry);
    free(directory);
    errno = libc_call_errno(status);
    return NULL;
  }
  *directory = (struct libc_directory){
    .handle = handle, .entry = entry, .name_capacity = INITIAL_NAME_CAPACITY,
  };
  return directory;
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
