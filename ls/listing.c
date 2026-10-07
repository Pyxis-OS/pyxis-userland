#include "ls.h"
#include "../common/directory.h"
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ls_free_listing(struct ls_listing *listing)
{
  for (size_t i = 0; i < listing->count; ++i) {
    free(listing->entries[i].name);
  }
  free(listing->entries);
  *listing = (struct ls_listing){0};
}

static int append_entry(struct ls_listing *listing, const char *name,
    const struct directory_enumerate_reply *reply)
{
  if (listing->count == listing->capacity) {
    size_t maximum = SIZE_MAX / sizeof(*listing->entries);
    if (listing->capacity > maximum / 2) {
      return -1;
    }
    size_t capacity = listing->capacity ? listing->capacity * 2 : 32;
    struct ls_entry *entries = realloc(listing->entries, capacity * sizeof(*entries));
    if (!entries) {
      return -1;
    }
    listing->entries = entries;
    listing->capacity = capacity;
  }
  char *owned_name = malloc(reply->name_size);
  if (!owned_name) {
    return -1;
  }
  memcpy(owned_name, name, reply->name_size);
  size_t length = strlen(name);
  enum ls_kind kind = LS_FILE;
  if (reply->kind == DIRECTORY_KIND_DIRECTORY) {
    kind = LS_DIRECTORY;
  } else if (length >= 4 && !strcmp(name + length - 4, ".pxe")) {
    kind = LS_PROGRAM;
  }
  listing->entries[listing->count++] = (struct ls_entry){
    .name = owned_name, .width = length + (kind == LS_DIRECTORY), .kind = kind,
  };
  return 0;
}

static int enumerate_entries(handle_t directory, const char *path,
    struct ls_listing *listing)
{
  size_t capacity = 128;
  char *name = malloc(capacity);
  if (!name) {
    report_directory_error("ls", path, CALL_NO_MEMORY);
    return -1;
  }
  int result = -1;
  struct directory_cursor cursor = {0};
  for (;;) {
    struct directory_enumerate_reply entry;
    enum call_status status = directory_enumerate(directory, &cursor, name, capacity, &entry);
    if (status != CALL_OK) {
      report_directory_error("ls", path, status);
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      result = 0;
      break;
    }
    if (entry.outcome == DIRECTORY_CHANGED) {
      fprintf(stderr, "ls: %s: Directory changed during listing\n", path);
      break;
    }
    if (entry.outcome == DIRECTORY_BUFFER_TOO_SMALL) {
      char *larger = realloc(name, entry.name_size);
      if (!larger) {
        report_directory_error("ls", path, CALL_NO_MEMORY);
        break;
      }
      name = larger;
      capacity = entry.name_size;
      continue; /* A short buffer leaves the cursor unchanged. */
    }
    if (append_entry(listing, name, &entry) != 0) {
      report_directory_error("ls", path, CALL_NO_MEMORY);
      break;
    }
    cursor = entry.cursor;
  }
  free(name);
  return result;
}

static int compare_entries(const void *left, const void *right)
{
  const struct ls_entry *a = left;
  const struct ls_entry *b = right;
  return strcmp(a->name, b->name);
}

static bool is_script(handle_t file)
{
  unsigned char prefix[2];
  size_t offset = 0;
  while (offset < sizeof(prefix)) {
    size_t read;
    if (file_read(file, offset, prefix + offset, sizeof(prefix) - offset, &read) != CALL_OK ||
        read == 0) {
      return false;
    }
    offset += read;
  }
  return prefix[0] == '#' && prefix[1] == '!';
}

static int read_details(handle_t directory, struct ls_listing *listing, bool long_listing)
{
  int result = 0;
  for (size_t i = 0; i < listing->count; ++i) {
    struct ls_entry *entry = &listing->entries[i];
    if (entry->kind == LS_DIRECTORY || (!long_listing && entry->kind == LS_PROGRAM)) {
      continue;
    }
    handle_t file;
    enum call_status status = directory_lookup(directory, entry->name,
        DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
    if (status != CALL_OK) {
      if (long_listing) {
        report_directory_error("ls", entry->name, status);
        result = -1;
      }
      continue;
    }
    if (long_listing) {
      status = file_size(file, &entry->size);
      entry->size_known = status == CALL_OK;
      if (!entry->size_known) {
        report_directory_error("ls", entry->name, status);
        result = -1;
      }
    }
    if (entry->kind == LS_FILE && is_script(file)) {
      entry->kind = LS_SCRIPT;
    }
    if (handle_close(file) != 0) {
      report_directory_error("ls", entry->name, CALL_BAD_HANDLE);
      result = -1;
    }
  }
  return result;
}

int ls_load_listing(const char *path, const struct ls_output *output,
    struct ls_listing *listing, bool *loaded)
{
  *loaded = false;
  bool details = output->terminal || output->long_listing;
  uint64_t rights = DIRECTORY_RIGHT_ENUMERATE;
  if (details) {
    rights |= DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES;
  }
  handle_t directory;
  enum call_status status = resolve_directory(path, rights, &directory);
  if (status == CALL_DENIED && details) {
    /* Enumeration alone must remain usable when file reads were withheld. */
    details = false;
    status = resolve_directory(path, DIRECTORY_RIGHT_ENUMERATE, &directory);
  }
  if (status != CALL_OK) {
    report_directory_error("ls", path, status);
    return -1;
  }

  int result = enumerate_entries(directory, path, listing);
  if (result == 0) {
    qsort(listing->entries, listing->count, sizeof(*listing->entries), compare_entries);
    *loaded = true;
    if (details) {
      result = read_details(directory, listing, output->long_listing);
    } else if (output->long_listing) {
      for (size_t i = 0; i < listing->count; ++i) {
        if (listing->entries[i].kind != LS_DIRECTORY) {
          report_directory_error("ls", path, CALL_DENIED);
          result = -1;
          break;
        }
      }
    }
  }
  if (handle_close(directory) != 0) {
    report_directory_error("ls", path, CALL_BAD_HANDLE);
    result = -1;
  }
  return result;
}
