#include "installer.h"

#include <directory.h>
#include <handle.h>
#include <pyxis_fs/npfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Installs from before the core archive (docs/wip/pkg-upgrade.md, step 2
 * task 3) copied their programs into bin/REVISION on the pool. Update removes
 * those copies once no retained boot entry binds them. */
#define BIN_RIGHTS DIRECTORY_CONTENT_RIGHTS
/* Installer settings, not ABI: bounded traversal and native names. */
#define TREE_MAX_DEPTH 32u
#define TREE_MAX_ENTRIES 65536u
#define TREE_NAME_BYTES (NPFS_NAME_MAX + 1)

static bool read_entry(handle_t directory, const struct directory_cursor *cursor,
    char name[TREE_NAME_BYTES], struct directory_enumerate_reply *entry)
{
  enum call_status status = directory_enumerate(directory, cursor, name, TREE_NAME_BYTES, entry);
  if (status != CALL_OK) {
    install_progress_pause();
    fprintf(stderr, "installer: cannot enumerate the bin volume (status %u)\n", status);
    return false;
  }
  if (entry->outcome == DIRECTORY_ENTRY || entry->outcome == DIRECTORY_END) {
    return true;
  }
  install_progress_pause();
  if (entry->outcome == DIRECTORY_BUFFER_TOO_SMALL) {
    fprintf(stderr, "installer: bin entry name exceeds %u bytes\n", NPFS_NAME_MAX);
  } else if (entry->outcome == DIRECTORY_CHANGED) {
    fputs("installer: bin volume changed during traversal\n", stderr);
  } else {
    fputs("installer: invalid bin enumeration reply\n", stderr);
  }
  return false;
}

struct tree_level {
  handle_t target;
  char name[TREE_NAME_BYTES];
};

/* Mutations invalidate enumeration cursors. Restart at zero after each child
 * removal; descend before removing directories, including empty ones. */
static bool clear_directory(handle_t directory)
{
  struct tree_level *levels = calloc(TREE_MAX_DEPTH + 1, sizeof(*levels));
  if (!levels) {
    return false;
  }
  levels[0].target = directory;
  unsigned depth = 0;
  size_t entries = 0;
  bool ok = true;
  while (ok) {
    struct tree_level *level = &levels[depth];
    const struct directory_cursor start = {0};
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    ok = read_entry(level->target, &start, name, &entry);
    if (!ok) {
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      if (!depth) {
        break;
      }
      handle_close(level->target);
      --depth;
      ok = directory_remove(levels[depth].target, level->name,
          DIRECTORY_KIND_DIRECTORY) == CALL_OK;
      if (ok) {
        install_progress_advance(0, 1);
      }
      continue;
    }
    if (++entries > TREE_MAX_ENTRIES) {
      ok = false;
      break;
    }
    if (entry.kind == DIRECTORY_KIND_FILE) {
      ok = directory_remove(level->target, name, DIRECTORY_KIND_FILE) == CALL_OK;
      if (ok) {
        install_progress_advance(0, 1);
      }
    } else if (entry.kind == DIRECTORY_KIND_DIRECTORY && depth < TREE_MAX_DEPTH) {
      handle_t child = HANDLE_INVALID;
      ok = directory_lookup(level->target, name, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &child) == CALL_OK;
      if (ok) {
        ++depth;
        levels[depth] = (struct tree_level){.target = child};
        strcpy(levels[depth].name, name);
      }
    } else {
      ok = false;
    }
  }
  while (depth) {
    handle_close(levels[depth--].target);
  }
  if (!ok) {
    install_progress_pause();
    fputs("installer: cannot clear old programs within traversal limits\n", stderr);
  }
  free(levels);
  return ok;
}

bool install_bin_cleanup(const struct install_disk *disk, uint64_t pool_partition,
    const char *previous)
{
  install_progress_begin("Opening program volume for cleanup", INSTALL_PROGRESS_NONE, NULL);
  handle_t root = HANDLE_INVALID;
  size_t entries = 0;
  bool changed = false;
  enum call_status status = disk_open_volume(disk->handle, pool_partition,
      INSTALL_BIN_VOLUME, BIN_RIGHTS, &root);
  if (status == CALL_NOT_FOUND) {
    install_progress_finish();
    return true;
  }
  bool ok = status == CALL_OK;
  if (ok) {
    install_progress_begin_major("Removing older program revision entries",
        INSTALL_PROGRESS_NONE, "entries");
  }
  struct directory_cursor cursor = {0};
  while (ok) {
    char name[TREE_NAME_BYTES];
    struct directory_enumerate_reply entry;
    ok = read_entry(root, &cursor, name, &entry);
    if (!ok || entry.outcome == DIRECTORY_END) {
      break;
    }
    if (++entries >= TREE_MAX_ENTRIES) {
      ok = false;
      break;
    }
    cursor = entry.cursor;
    if (entry.kind != DIRECTORY_KIND_DIRECTORY || (previous && !strcmp(name, previous))) {
      continue;
    }
    handle_t directory = HANDLE_INVALID;
    ok = directory_lookup(root, name, DIRECTORY_KIND_DIRECTORY, BIN_RIGHTS,
          &directory) == CALL_OK && clear_directory(directory);
    if (directory != HANDLE_INVALID) {
      handle_close(directory);
    }
    ok = ok && directory_remove(root, name, DIRECTORY_KIND_DIRECTORY) == CALL_OK;
    if (ok) {
      changed = true;
      install_progress_advance(0, 1);
      install_progress_detail("Removed old programs bin://%s", name);
      cursor = (struct directory_cursor){0};
    }
  }
  if (ok && changed) {
    install_progress_begin_major("Flushing program cleanup", INSTALL_PROGRESS_NONE, NULL);
    ok = directory_sync(root) == CALL_OK;
  }
  install_progress_finish();
  if (root != HANDLE_INVALID) {
    install_progress_begin("Closing program cleanup directory", INSTALL_PROGRESS_NONE, NULL);
    handle_close(root);
  }
  install_progress_finish();
  return ok;
}
