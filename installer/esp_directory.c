#include "esp_directory.h"

#include <pyxis_fs/fat32.h>

static unsigned ascii_upper(unsigned byte)
{
  return byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
}

static bool same_name(const char *canonical, const char *requested)
{
  for (size_t i = 0;; ++i) {
    unsigned left = (unsigned char)canonical[i], right = (unsigned char)requested[i];
    if (ascii_upper(left) != ascii_upper(right)) {
      return false;
    }
    if (!left) {
      return true;
    }
  }
}

enum call_status install_esp_child_open(handle_t parent, const char *name,
    uint64_t kind, uint64_t rights, bool create, handle_t *child)
{
  if (!child) {
    return CALL_BAD_REQUEST;
  }
  *child = HANDLE_INVALID;
  if (!name || !*name || (kind != DIRECTORY_KIND_DIRECTORY && kind != DIRECTORY_KIND_FILE)) {
    return CALL_BAD_REQUEST;
  }
  struct directory_cursor cursor = {0};
  for (;;) {
    char canonical[FAT32_NAME_MAX + 1];
    struct directory_enumerate_reply entry;
    enum call_status status = directory_enumerate(parent, &cursor,
        canonical, sizeof(canonical), &entry);
    if (status != CALL_OK) {
      return status;
    }
    switch (entry.outcome) {
    case DIRECTORY_END:
      return create ? directory_create(parent, name, kind, rights, child) :
        CALL_NOT_FOUND;
    case DIRECTORY_CHANGED:
      return CALL_BUSY;
    case DIRECTORY_BUFFER_TOO_SMALL:
      return CALL_BUFFER_TOO_SMALL;
    case DIRECTORY_ENTRY:
      if (same_name(canonical, name)) {
        return entry.kind == kind ?
          directory_lookup(parent, canonical, kind, rights, child) :
          CALL_WRONG_TYPE;
      }
      cursor = entry.cursor;
      break;
    default:
      return CALL_BAD_REQUEST;
    }
  }
}
