#include "store.h"
#include <clock.h>
#include <file.h>
#include <handle.h>
#include <package_maintenance.h>
#include <stdio.h>
#include <string.h>

#define MAINTENANCE_NS UINT64_C(5000000000)
#define MAINTENANCE_DEPTH_MAX 128
#define GC_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_REMOVE)

struct maintenance {
  handle_t clock;
  uint64_t deadline;
  const char *reason;
};

static bool within_budget(struct maintenance *work)
{
  uint64_t now;
  if (clock_now(work->clock, &now) != CALL_OK) {
    work->reason = "clock unavailable";
    return false;
  }
  if (now >= work->deadline) {
    work->reason = "five-second maintenance budget exhausted";
    return false;
  }
  return true;
}

static bool native_object(struct maintenance *work, handle_t object, uint64_t protocol)
{
  struct handle_info info;
  if (!within_budget(work)) {
    return false;
  }
  if (handle_query(object, &info) != CALL_OK || info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != protocol || info.transport) {
    work->reason = "cleanup requires native files and directories";
    return false;
  }
  return true;
}

static bool open_directory(struct maintenance *work, handle_t parent,
    const char *name, handle_t *child)
{
  *child = HANDLE_INVALID;
  if (!within_budget(work)) {
    return false;
  }
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_DIRECTORY,
      GC_RIGHTS, child);
  if (status != CALL_OK) {
    work->reason = "cannot open cleanup directory";
    return false;
  }
  if (!native_object(work, *child, PROTOCOL_DIRECTORY)) {
    handle_close(*child);
    *child = HANDLE_INVALID;
    return false;
  }
  return true;
}

static bool enumerate(struct maintenance *work, handle_t directory,
    struct directory_cursor *cursor, char name[256], struct directory_enumerate_reply *reply)
{
  for (;;) {
    if (!within_budget(work)) {
      return false;
    }
    if (directory_enumerate(directory, cursor, name, 256, reply) != CALL_OK) {
      work->reason = "cannot enumerate cleanup directory";
      return false;
    }
    if (reply->outcome == DIRECTORY_CHANGED) {
      *cursor = (struct directory_cursor){0};
      continue;
    }
    if (reply->outcome != DIRECTORY_ENTRY && reply->outcome != DIRECTORY_END) {
      work->reason = "cleanup directory has an unsupported entry name";
      return false;
    }
    *cursor = reply->cursor;
    if (reply->outcome == DIRECTORY_ENTRY && reply->kind != DIRECTORY_KIND_FILE &&
        reply->kind != DIRECTORY_KIND_DIRECTORY) {
      work->reason = "cleanup directory contains a link or unsupported entry kind";
      return false;
    }
    return true;
  }
}

static bool remove_name(struct maintenance *work, handle_t parent,
    const char *name, uint64_t kind)
{
  if (!within_budget(work)) {
    return false;
  }
  if (directory_remove(parent, name, kind) != CALL_OK) {
    work->reason = "cannot remove abandoned entry";
    return false;
  }
  return true;
}

static bool clear_directory(struct maintenance *work, handle_t directory, unsigned depth);

static bool remove_entry(struct maintenance *work, handle_t parent,
    const char *name, uint64_t kind, unsigned depth)
{
  if (depth >= MAINTENANCE_DEPTH_MAX) {
    work->reason = "abandoned tree exceeds cleanup depth bound";
    return false;
  }
  handle_t child = HANDLE_INVALID;
  if (kind == DIRECTORY_KIND_DIRECTORY) {
    if (!open_directory(work, parent, name, &child)) {
      return false;
    }
    bool ok = clear_directory(work, child, depth + 1);
    handle_close(child);
    if (!ok) {
      return false;
    }
  } else {
    if (!within_budget(work)) {
      return false;
    }
    if (directory_lookup(parent, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &child) != CALL_OK) {
      work->reason = "cannot inspect abandoned file";
      return false;
    }
    bool ok = native_object(work, child, PROTOCOL_FILE);
    handle_close(child);
    if (!ok) {
      return false;
    }
  }
  return remove_name(work, parent, name, kind);
}

static bool clear_directory(struct maintenance *work, handle_t directory, unsigned depth)
{
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply reply;
    if (!enumerate(work, directory, &cursor, name, &reply)) {
      return false;
    }
    if (reply.outcome == DIRECTORY_END) {
      return true;
    }
    if (!remove_entry(work, directory, name, reply.kind, depth)) {
      return false;
    }
    /* Our removal invalidates the live cursor. Every remaining entry is
     * removable here, so starting again cannot skip a surviving child. */
    cursor = (struct directory_cursor){0};
  }
}

static bool record_number(const char *name, uint64_t *number)
{
  const char *tail = name;
  uint64_t value = 0;
  if (*tail < '1' || *tail > '9') {
    return false;
  }
  while (*tail >= '0' && *tail <= '9') {
    unsigned digit = (unsigned)(*tail++ - '0');
    if (value > ((uint64_t)INT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  if (strcmp(tail, ".lua") && strcmp(tail, ".previous") && strcmp(tail, ".commands")) {
    return false;
  }
  *number = value;
  return true;
}

static bool collect_records(struct maintenance *work, handle_t packages,
    uint64_t current, uint64_t previous)
{
  handle_t sets;
  if (!open_directory(work, packages, "sets", &sets)) {
    return false;
  }
  bool ok = true;
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply reply;
    if (!enumerate(work, sets, &cursor, name, &reply)) {
      ok = false;
      break;
    }
    if (reply.outcome == DIRECTORY_END) {
      break;
    }
    uint64_t number;
    if (reply.kind != DIRECTORY_KIND_FILE || !record_number(name, &number)) {
      work->reason = "unexpected immutable set record name or kind";
      ok = false;
      break;
    }
    if (number != current && number != previous) {
      if (!remove_entry(work, sets, name, reply.kind, 0)) {
        ok = false;
        break;
      }
      cursor = (struct directory_cursor){0};
    }
  }
  handle_close(sets);
  return ok;
}

static bool retained(const struct pkg_selection *selection, const char *id, const char *version)
{
  for (size_t i = 0; i < selection->count; ++i) {
    if (!strcmp(selection->entries[i].id, id) &&
        !strcmp(selection->entries[i].version, version)) {
      return true;
    }
  }
  return false;
}

static bool collect_versions(struct maintenance *work, handle_t id_directory,
    const char *id, const struct pkg_selection *current, const struct pkg_selection *previous)
{
  struct directory_cursor cursor = {0};
  for (;;) {
    char version[256];
    struct directory_enumerate_reply reply;
    if (!enumerate(work, id_directory, &cursor, version, &reply)) {
      return false;
    }
    if (reply.outcome == DIRECTORY_END) {
      return true;
    }
    if (reply.kind != DIRECTORY_KIND_DIRECTORY) {
      work->reason = "package store version is not a directory";
      return false;
    }
    if (!retained(current, id, version) && !retained(previous, id, version)) {
      if (!remove_entry(work, id_directory, version, reply.kind, 0)) {
        return false;
      }
      cursor = (struct directory_cursor){0};
    }
  }
}

static bool collect_store(struct maintenance *work, handle_t packages,
    const struct pkg_selection *current, const struct pkg_selection *previous)
{
  handle_t store;
  if (!open_directory(work, packages, "store", &store)) {
    return false;
  }
  bool ok = true;
  struct directory_cursor cursor = {0};
  for (;;) {
    char id[256];
    struct directory_enumerate_reply reply;
    if (!enumerate(work, store, &cursor, id, &reply)) {
      ok = false;
      break;
    }
    if (reply.outcome == DIRECTORY_END) {
      break;
    }
    if (reply.kind != DIRECTORY_KIND_DIRECTORY || !package_id_valid(id)) {
      work->reason = "package store ID is not a valid directory";
      ok = false;
      break;
    }
    handle_t id_directory;
    if (!open_directory(work, store, id, &id_directory)) {
      ok = false;
      break;
    }
    ok = collect_versions(work, id_directory, id, current, previous);
    handle_close(id_directory);
    if (!ok) {
      break;
    }
  }
  handle_close(store);
  return ok;
}

static bool collect_packages(struct maintenance *work, handle_t packages)
{
  if (!native_object(work, packages, PROTOCOL_DIRECTORY)) {
    return false;
  }
  struct pkg_selection current = {0}, previous = {0};
  uint64_t previous_number = 0;
  bool ok = within_budget(work) &&
      pkg_selection_load(packages, true, 0, &current, &work->reason);
  if (ok && !current.number) {
    work->reason = "no current set; retention evidence unavailable";
    ok = false;
  }
  if (ok) {
    ok = within_budget(work) &&
        pkg_predecessor(packages, current.number, &previous_number, &work->reason);
  }
  if (ok && previous_number) {
    ok = within_budget(work) &&
        pkg_selection_load(packages, false, previous_number, &previous, &work->reason);
  }
  /* Only these two records establish retention. The predecessor's own sidecar
   * stays with it, but must not extend retention recursively. */
  if (ok) {
    ok = collect_records(work, packages, current.number, previous_number) &&
        collect_store(work, packages, &current, &previous);
  }
  if (ok) {
    handle_t staging;
    ok = open_directory(work, packages, "staging", &staging);
    if (ok) {
      ok = clear_directory(work, staging, 0);
      handle_close(staging);
    }
  }
  pkg_selection_free(&previous);
  pkg_selection_free(&current);
  return ok;
}

void package_boot_maintain(handle_t packages, handle_t http_work, handle_t clock)
{
  if (packages == HANDLE_INVALID && http_work == HANDLE_INVALID) {
    return;
  }
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - MAINTENANCE_NS) {
    puts("boot-init: package maintenance skipped: clock unavailable");
    return;
  }
  struct maintenance work = {.clock = clock, .deadline = now + MAINTENANCE_NS};
  if (http_work != HANDLE_INVALID &&
      (!native_object(&work, http_work, PROTOCOL_DIRECTORY) ||
       !clear_directory(&work, http_work, 0))) {
    printf("boot-init: private HTTP cleanup stopped: %s\n", work.reason);
  }
  work.reason = NULL;
  if (packages != HANDLE_INVALID && !collect_packages(&work, packages)) {
    printf("boot-init: package collection skipped or stopped: %s\n",
        work.reason ? work.reason : "retention evidence unavailable");
  }
}
