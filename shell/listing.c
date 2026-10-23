#include "shell.h"
#include <directory.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

/* A directory that keeps changing while it is listed is given up on. */
#define LISTING_RESTARTS 8

void shell_listing_close(struct shell_listing *listing)
{
  free(listing->names);
  free(listing->items);
  *listing = (struct shell_listing){0};
}

static void listing_clear(struct shell_listing *listing)
{
  listing->name_bytes = 0;
  listing->count = 0;
}

static bool listing_add(struct shell_listing *listing, const char *name, bool directory)
{
  size_t length = strlen(name) + 1;
  if (length > SIZE_MAX - listing->name_bytes) {
    return false;
  }
  if (listing->name_bytes + length > listing->name_capacity) {
    size_t capacity = listing->name_capacity ? listing->name_capacity : 256;
    while (capacity < listing->name_bytes + length) {
      if (capacity > SIZE_MAX / 2) {
        return false;
      }
      capacity *= 2;
    }
    char *larger = realloc(listing->names, capacity);
    if (!larger) {
      return false;
    }
    listing->names = larger;
    listing->name_capacity = capacity;
  }
  if (listing->count == listing->item_capacity) {
    size_t capacity = listing->item_capacity ? listing->item_capacity : 32;
    if (capacity > SIZE_MAX / 2 / sizeof(*listing->items)) {
      return false;
    }
    if (listing->item_capacity) {
      capacity *= 2;
    }
    struct shell_listing_item *larger = realloc(listing->items,
        capacity * sizeof(*larger));
    if (!larger) {
      return false;
    }
    listing->items = larger;
    listing->item_capacity = capacity;
  }
  memcpy(listing->names + listing->name_bytes, name, length);
  listing->items[listing->count++] =
      (struct shell_listing_item){.offset = listing->name_bytes, .directory = directory};
  listing->name_bytes += length;
  return true;
}

const char *shell_listing_name(const struct shell_listing *listing, size_t index)
{
  return listing->names + listing->items[index].offset;
}

enum call_status shell_list_directory(struct shell *shell, const char *path, size_t limit,
    struct shell_listing *listing)
{
  *listing = (struct shell_listing){0};
  handle_t handle;
  enum call_status status = shell_open_directory(shell, path, DIRECTORY_RIGHT_ENUMERATE,
      &handle);
  if (status != CALL_OK) {
    return status;
  }
  size_t capacity = 128;
  char *name = malloc(capacity);
  if (!name) {
    handle_close(handle);
    return CALL_NO_MEMORY;
  }
  struct directory_cursor cursor = {0};
  size_t restarts = 0;
  for (;;) {
    struct directory_enumerate_reply entry;
    status = directory_enumerate(handle, &cursor, name, capacity, &entry);
    if (status != CALL_OK || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (entry.outcome == DIRECTORY_CHANGED) {
      /* The listing is live: start again rather than mix two states. */
      if (++restarts > LISTING_RESTARTS) {
        status = CALL_BUSY;
        break;
      }
      listing_clear(listing);
      cursor = (struct directory_cursor){0};
      continue;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = entry.name_size > capacity ? realloc(name, entry.name_size) : NULL;
      if (!larger) {
        status = CALL_NO_MEMORY;
        break;
      }
      name = larger;
      capacity = entry.name_size;
      continue;
    }
    cursor = entry.cursor;
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
      continue;
    }
    if (limit && listing->count >= limit) {
      status = CALL_LIMIT;
      break;
    }
    if (!listing_add(listing, name, entry.kind == DIRECTORY_KIND_DIRECTORY)) {
      status = CALL_NO_MEMORY;
      break;
    }
  }
  free(name);
  handle_close(handle);
  if (status != CALL_OK) {
    shell_listing_close(listing);
  }
  return status;
}
