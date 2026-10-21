#include "archive.h"
#include "commands.h"
#include "fetch.h"
#include "progress.h"
#include "store.h"
#include "../common/bootstrap_environment.h"
#include "../xfer/hash.h"
#include <clock.h>
#include <console.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <package_maintenance.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

struct offered_package {
  struct pkg_entry entry;
  size_t repository;
};

struct client {
  handle_t packages, clock, config, config_write, management, system_info;
  /* What the running system provides, read once on first use. */
  struct package_provided_abi provided;
  bool provided_loaded, provided_readable;
  /* Builds for an ABI this system does not provide, or for any ABI when the
   * running one is unreadable: not offered, only reported. */
  struct pkg_identity *hidden;
  size_t hidden_count;
  char message[2048];
  struct terminal terminal;
  handle_t passthrough;
  struct pkg_fetch_authority fetch;
  struct pkg_repositories repositories;
  struct offered_package *offered;
  size_t offered_count;
  uint64_t timeout;
  char detail[RESOURCE_DETAIL_MAX_BYTES + 1];
  const char *reason;
  enum call_status cleanup_status;
};

/* The literal parser imposes the stricter eventual set-record bound. */
#define PKG_SELECTION_MAX 8192

static struct client client;

/* Preparation keeps ordinary foreground interruption and never scans stdin.
 * Only the confirmed visibility switch holds passthrough until it settles. */
static void release_publish_guard(void)
{
  if (client.passthrough != HANDLE_INVALID) {
    handle_close(client.passthrough);
    client.passthrough = HANDLE_INVALID;
  }
}

static bool fail(const char *reason)
{
  client.reason = reason;
  return false;
}

static const struct package_provided_abi *provided_abi(void)
{
  if (!client.provided_loaded) {
    client.provided_loaded = true;
    client.provided_readable = client.system_info != HANDLE_INVALID &&
        package_provided_abi(client.system_info, &client.provided) == CALL_OK;
  }
  return &client.provided;
}

/* A program or toolchain package is usable only on a system that provides its
 * ABI. Refusals name both sides; INSTALLED adds the way out for a stored one. */
static bool abi_check(const char *id, const char *version, const char *kind,
    const char *abi, bool installed)
{
  if (!pkg_kind_bound(kind)) {
    return true;
  }
  const struct package_provided_abi *provided = provided_abi();
  char names[PACKAGE_PROVIDED_ABI_MAX * (PACKAGE_ABI_MAX + 2) + 8];
  package_provided_abi_text(provided, names, sizeof(names));
  char remedy[2 * PACKAGE_ID_MAX + 80] = "";
  if (installed) {
    snprintf(remedy, sizeof(remedy),
        "; pkg install %s takes a build for this system, pkg remove %s drops it", id, id);
  }
  if (!client.provided_readable) {
    snprintf(client.message, sizeof(client.message),
        "%s %s needs an ABI check, but the running system's ABI is unreadable (no system_info)",
        id, version);
  } else if (!package_abi_provided(provided, abi)) {
    snprintf(client.message, sizeof(client.message),
        "%s %s needs ABI %s; this system provides %s%s", id, version, abi, names, remedy);
  } else {
    return true;
  }
  return fail(client.message);
}

static void append(char *text, size_t capacity, const char *separator, const char *word)
{
  size_t used = strlen(text);
  if (used < capacity) {
    snprintf(text + used, capacity - used, "%s%s", used ? separator : "", word);
  }
}

/* Every stored package in SELECTION that this system cannot run, found from
 * manifests alone, refused in one message whose commands clear them all at once;
 * one refusal per package would send the user round in circles. SKIP lists IDs
 * this operation fetches anew. COMMAND and ARGV, for install and remove, are
 * repeated in the suggested command. */
static bool stale_check(const struct pkg_selection *selection, const struct pkg_identity *skip,
    size_t skip_count, const char *command, int argc, char **argv)
{
  const struct package_provided_abi *provided = provided_abi();
  char stale[1024] = "", details[1024] = "";
  size_t count = 0;
  for (size_t i = 0; i < selection->count; ++i) {
    const struct pkg_identity *identity = &selection->entries[i];
    bool skipped = false;
    for (size_t s = 0; s < skip_count; ++s) {
      skipped |= !strcmp(skip[s].id, identity->id);
    }
    handle_t tree;
    struct pkg_manifest manifest;
    if (skipped || !pkg_tree_open(client.packages, identity, PKG_READ_RIGHTS, &tree, &client.reason)) {
      client.reason = NULL;
      continue;
    }
    bool read = pkg_tree_manifest(tree, &manifest, &client.reason);
    handle_close(tree);
    client.reason = NULL;
    if (read && pkg_kind_bound(manifest.kind) && (!client.provided_readable ||
        !package_abi_provided(provided, manifest.abi))) {
      char detail[PACKAGE_ID_MAX + PACKAGE_ABI_MAX + 32];
      snprintf(detail, sizeof(detail), "%s (needs ABI %s)", identity->id, manifest.abi);
      append(stale, sizeof(stale), " ", identity->id);
      append(details, sizeof(details), ", ", detail);
      ++count;
    }
    if (read) {
      pkg_manifest_free(&manifest);
    }
  }
  if (!count) {
    return true;
  }
  if (!client.provided_readable) {
    snprintf(client.message, sizeof(client.message),
        "cannot check the ABI of %s: this system's ABI is unreadable (no system_info grant)", stale);
    return fail(client.message);
  }
  char install[1100] = "", remove[1100] = "";
  bool installing = !strcmp(command, "install"), removing = !strcmp(command, "remove");
  for (int i = 0; (installing || removing) && i < argc; ++i) {
    append(installing ? install : remove, sizeof(install), " ", argv[i]);
  }
  append(install, sizeof(install), " ", stale);
  append(remove, sizeof(remove), " ", stale);
  char names[PACKAGE_PROVIDED_ABI_MAX * (PACKAGE_ABI_MAX + 2) + 8];
  package_provided_abi_text(provided, names, sizeof(names));
  snprintf(client.message, sizeof(client.message),
      "%zu installed package%s not built for this system, which provides %s: %s. "
      "Replace with: pkg install %s. Or drop with: pkg remove %s",
      count, count == 1 ? " is" : "s are", names, details, install, remove);
  return fail(client.message);
}

static bool release_writer_lock(handle_t packages, bool ok)
{
  enum call_status status = directory_remove(packages, PKG_LOCK, DIRECTORY_KIND_DIRECTORY);
  if (status == CALL_OK) status = directory_sync(packages);
  if (status == CALL_OK) return ok;
  client.cleanup_status = status;
  if (ok) client.reason = NULL;
  else if (!client.reason) client.reason = "operation failed";
  return false;
}

static size_t find_package(const struct pkg_selection *selection, const char *id)
{
  for (size_t i = 0; i < selection->count; ++i) {
    if (!strcmp(selection->entries[i].id, id)) {
      return i;
    }
  }
  return SIZE_MAX;
}

static bool select_version(struct pkg_selection *selection,
    const struct pkg_identity *identity, bool replace)
{
  size_t index = find_package(selection, identity->id);
  if (index != SIZE_MAX) {
    if (!replace && strcmp(selection->entries[index].version, identity->version)) {
      return fail("incompatible exact dependencies in the proposed set");
    }
    selection->entries[index] = *identity;
    return true;
  }
  if (selection->count >= PKG_SELECTION_MAX) {
    return fail("package selection exceeds metadata budget");
  }
  if (selection->count == selection->capacity) {
    size_t capacity = selection->capacity ? selection->capacity * 2 : 8;
    if (capacity > PKG_SELECTION_MAX) capacity = PKG_SELECTION_MAX;
    struct pkg_identity *entries = realloc(selection->entries, capacity * sizeof(*entries));
    if (!entries) {
      return fail("cannot grow package selection");
    }
    selection->entries = entries;
    selection->capacity = capacity;
  }
  selection->entries[selection->count++] = *identity;
  return true;
}

static void print_selection(const struct pkg_selection *selection)
{
  printf("set %llu: %zu packages\n", (unsigned long long)selection->number,
      selection->count);
  for (size_t i = 0; i < selection->count; ++i) {
    printf("  %s@%s\n", selection->entries[i].id, selection->entries[i].version);
  }
}

static bool read_snapshot(handle_t file, char **bytes, size_t *size)
{
  *bytes = NULL;
  *size = 0;
  uint64_t length;
  if (file_size(file, &length) != CALL_OK || length > LITERAL_SOURCE_LIMIT) {
    return fail("repository index exceeds its metadata budget");
  }
  char *buffer = malloc(length ? length : 1);
  if (!buffer) {
    return fail("cannot allocate repository index");
  }
  size_t offset = 0;
  while (offset < length) {
    size_t count;
    if (file_read(file, offset, buffer + offset,
          length - offset, &count) != CALL_OK || !count) {
      free(buffer);
      return fail("repository index read failed or cancelled");
    }
    offset += count;
  }
  *bytes = buffer;
  *size = length;
  return true;
}

/* progress, when given, has its open line closed before the failure is printed. */
static bool fetch(const char *base, const char *relative, bool index, uint64_t size,
    struct pkg_progress *progress, handle_t *file)
{
  enum call_status status = pkg_fetch(&client.fetch, base, relative, index, size,
      client.timeout, file, client.detail);
  if (status != CALL_OK) {
    if (progress) {
      pkg_progress_abort(progress);
    }
    fprintf(stderr, "pkg: %s%s: %s (fetch status %u)\n",
        base, relative, client.detail, status);
    return fail("no complete download; active set unchanged");
  }
  return true;
}

static bool load_repositories(void)
{
  if (pkg_repositories_load(client.config, &client.repositories, &client.reason) != CALL_OK) {
    return false;
  }
  if (!client.repositories.count) {
    return fail("no repositories configured; use pkg repository add URL (HTTP/HTTPS base; final / optional)");
  }
  for (size_t r = 0; r < client.repositories.count; ++r) {
    handle_t file;
    printf("pkg: reading repository %zu/%zu\n", r + 1, client.repositories.count);
    if (!fetch(client.repositories.urls[r], "index.lua", true, 0, NULL, &file)) {
      return false;
    }
    char *bytes = NULL;
    size_t size = 0;
    struct pkg_entry *entries = NULL;
    size_t count = 0;
    bool ok = read_snapshot(file, &bytes, &size) &&
        pkg_index_parse(bytes, size, &entries, &count, &client.reason);
    free(bytes);
    handle_close(file);
    if (!ok) {
      return false;
    }
    const struct package_provided_abi *provided = provided_abi();
    size_t usable = 0;
    for (size_t i = 0; i < count; ++i) {
      if (pkg_kind_bound(entries[i].kind) && (!client.provided_readable ||
          !package_abi_provided(provided, entries[i].abi))) {
        struct pkg_identity *hidden = realloc(client.hidden,
            (client.hidden_count + 1) * sizeof(*hidden));
        if (!hidden) {
          free(entries);
          return fail("cannot allocate repository catalog");
        }
        client.hidden = hidden;
        client.hidden[client.hidden_count++] = entries[i].identity;
        continue;
      }
      entries[usable++] = entries[i];
    }
    count = usable;
    for (size_t i = 0; i < count; ++i) {
      for (size_t j = 0; j < client.offered_count; ++j) {
        if (client.offered[j].repository != r &&
            !strcmp(client.offered[j].entry.identity.id, entries[i].identity.id)) {
          free(entries);
          return fail("a package ID is offered by more than one repository");
        }
      }
    }
    size_t total = client.offered_count + count;
    struct offered_package *offered = realloc(client.offered,
        (total ? total : 1) * sizeof(*offered));
    if (!offered) {
      free(entries);
      return fail("cannot allocate repository catalog");
    }
    client.offered = offered;
    for (size_t i = 0; i < count; ++i) {
      client.offered[client.offered_count++] = (struct offered_package){entries[i], r};
    }
    free(entries);
  }
  return true;
}

static const struct offered_package *offering(const char *id, const char *version)
{
  const struct offered_package *found = NULL;
  for (size_t i = 0; i < client.offered_count; ++i) {
    const struct pkg_identity *identity = &client.offered[i].entry.identity;
    if (!strcmp(identity->id, id) && (!version || !strcmp(identity->version, version))) {
      found = &client.offered[i];
    }
  }
  return found;
}

static size_t hidden_builds(const char *id, const char *version)
{
  size_t count = 0;
  for (size_t i = 0; i < client.hidden_count; ++i) {
    count += !strcmp(client.hidden[i].id, id) && (!version || !strcmp(client.hidden[i].version, version));
  }
  return count;
}

static bool request(const char *text, struct pkg_identity *identity, bool *explicit_version)
{
  const char *at = strchr(text, '@');
  size_t length = at ? (size_t)(at - text) : strlen(text);
  if (!length || length > PACKAGE_ID_MAX) {
    return fail("invalid package ID");
  }
  memcpy(identity->id, text, length);
  identity->id[length] = 0;
  if (!package_id_valid(identity->id) || (at && !pkg_version_valid(at + 1))) {
    return fail("invalid package ID or version");
  }
  *explicit_version = at != NULL;
  if (at) {
    strcpy(identity->version, at + 1);
  } else {
    identity->version[0] = 0;
  }
  return true;
}

static bool verify_version(const struct pkg_identity *identity, struct pkg_manifest *manifest)
{
  handle_t tree;
  if (!pkg_tree_open(client.packages, identity, PKG_READ_RIGHTS, &tree, &client.reason)) {
    return false;
  }
  /* The ABI refusal needs only the manifest, so it comes before hashing a
   * possibly large tree. */
  struct pkg_manifest stored;
  bool ok = pkg_tree_manifest(tree, &stored, &client.reason) &&
      abi_check(identity->id, identity->version, stored.kind, stored.abi, true);
  pkg_manifest_free(&stored);
  if (ok) {
    printf("pkg: verifying %s@%s\n", identity->id, identity->version);
    ok = pkg_tree_verify(tree, NULL, manifest, &client.reason);
  }
  handle_close(tree);
  if (ok && (strcmp(manifest->identity.id, identity->id) ||
      strcmp(manifest->identity.version, identity->version))) {
    pkg_manifest_free(manifest);
    return fail("stored manifest does not match its package directory");
  }
  return ok;
}

/* A member carried over unchanged into a new set was verified when it was
 * installed, and published trees are immutable: only its stored manifest is
 * read and checked, never its tree. pkg verify still hashes every member. */
static bool trust_version(const struct pkg_identity *identity, struct pkg_manifest *manifest)
{
  handle_t tree;
  if (!pkg_tree_open(client.packages, identity, PKG_READ_RIGHTS, &tree, &client.reason)) {
    return false;
  }
  bool ok = pkg_tree_manifest(tree, manifest, &client.reason);
  handle_close(tree);
  if (ok && (strcmp(manifest->identity.id, identity->id) ||
      strcmp(manifest->identity.version, identity->version))) {
    pkg_manifest_free(manifest);
    return fail("stored manifest does not match its package directory");
  }
  if (ok && !abi_check(identity->id, identity->version, manifest->kind, manifest->abi, true)) {
    pkg_manifest_free(manifest);
    return false;
  }
  return ok;
}

static bool install_version(const struct offered_package *offered, struct pkg_manifest *manifest)
{
  const struct pkg_identity *identity = &offered->entry.identity;
  /* Console output is redrawn in place; pipes, files and other captures get
   * one plain line per phase. */
  struct pkg_progress progress;
  pkg_progress_init(&progress, client.clock, client.terminal.output != HANDLE_INVALID,
      identity->id, identity->version);
  struct pkg_archive_progress observer = { pkg_progress_archive, &progress };
  pkg_progress_fetch_begin(&progress, offered->entry.size);
  handle_t snapshot;
  if (!fetch(client.repositories.urls[offered->repository], offered->entry.archive,
        false, offered->entry.size, &progress, &snapshot)) {
    return false;
  }
  pkg_progress_fetch_end(&progress, offered->entry.size);
  handle_t existing = HANDLE_INVALID;
  bool exists = pkg_tree_open(client.packages, identity, PKG_READ_RIGHTS,
      &existing, &client.reason);
  client.reason = NULL;
  if (exists) {
    struct pkg_manifest desired = {0};
    bool ok = pkg_archive(snapshot, &offered->entry, HANDLE_INVALID, &desired, &observer,
        &client.reason) &&
        pkg_tree_verify(existing, &desired, manifest, &client.reason);
    pkg_progress_abort(&progress);
    pkg_manifest_free(&desired);
    handle_close(existing);
    handle_close(snapshot);
    return ok;
  }
  handle_t staging = HANDLE_INVALID, destination = HANDLE_INVALID;
  handle_t store = HANDLE_INVALID, id = HANDLE_INVALID;
  uint64_t now;
  bool ok = pkg_directory(client.packages, "staging", true, PKG_WRITE_RIGHTS,
      &staging, &client.reason);
  char name[64];
  if (ok && clock_now(client.clock, &now) != CALL_OK) {
    ok = fail("cannot name a fresh staging attempt");
  }
  if (ok) {
    snprintf(name, sizeof(name), "attempt-%llu", (unsigned long long)now);
    if (directory_create(staging, name, DIRECTORY_KIND_DIRECTORY, PKG_WRITE_RIGHTS,
          &destination) != CALL_OK) {
      ok = fail("cannot exclusively create a staging attempt");
    }
  }
  ok = ok && pkg_archive(snapshot, &offered->entry, destination, manifest, &observer,
      &client.reason);
  pkg_progress_abort(&progress);
  handle_close(snapshot);
  ok = ok && pkg_directory(client.packages, "store", true, PKG_WRITE_RIGHTS,
      &store, &client.reason);
  ok = ok && pkg_directory(store, identity->id, true, PKG_WRITE_RIGHTS, &id, &client.reason);
  if (ok && (directory_rename(staging, name, id, identity->version,
          DIRECTORY_RENAME_NO_REPLACE, DIRECTORY_KIND_DIRECTORY) != CALL_OK ||
      directory_sync(client.packages) != CALL_OK)) {
    ok = fail("cannot durably publish the verified immutable package tree");
  }
  if (!ok) {
    pkg_manifest_free(manifest);
  }
  if (id != HANDLE_INVALID) handle_close(id);
  if (store != HANDLE_INVALID) handle_close(store);
  if (destination != HANDLE_INVALID) handle_close(destination);
  if (staging != HANDLE_INVALID) handle_close(staging);
  return ok;
}

static bool check_dependencies(const struct pkg_selection *selection,
    const struct pkg_manifest *manifest)
{
  for (size_t d = 0; d < manifest->dependency_count; ++d) {
    const struct pkg_identity *dependency = &manifest->dependencies[d];
    size_t index = find_package(selection, dependency->id);
    if (index == SIZE_MAX || strcmp(selection->entries[index].version, dependency->version)) {
      return fail("proposed set does not satisfy every exact dependency");
    }
  }
  return true;
}

/* Members of SELECTION that CARRIED (may be NULL) holds at the same version are
 * trusted; every other member is hashed. */
static bool verify_selection(const struct pkg_selection *selection,
    const struct pkg_selection *carried)
{
  for (size_t i = 0; i < selection->count; ++i) {
    const struct pkg_identity *identity = &selection->entries[i];
    size_t index = carried ? find_package(carried, identity->id) : SIZE_MAX;
    bool unchanged = index != SIZE_MAX &&
        !strcmp(carried->entries[index].version, identity->version);
    struct pkg_manifest manifest = {0};
    if (!(unchanged ? trust_version(identity, &manifest) : verify_version(identity, &manifest))) {
      return false;
    }
    bool ok = check_dependencies(selection, &manifest);
    pkg_manifest_free(&manifest);
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool prepare_install(struct pkg_selection *selection, int argc, char **argv)
{
  if (!load_repositories()) {
    return false;
  }
  size_t initial_count = selection->count;
  /* A bounded list identifies explicit requests, which are verified against the
   * repository even if an immutable copy already exists locally. */
  struct pkg_identity *requested = calloc(argc, sizeof(*requested));
  if (!requested) {
    return fail("cannot allocate package requests");
  }
  bool ok = true;
  for (int i = 0; ok && i < argc; ++i) {
    struct pkg_identity identity;
    bool explicit_version;
    ok = request(argv[i], &identity, &explicit_version);
    const struct offered_package *entry = ok ?
        offering(identity.id, explicit_version ? identity.version : NULL) : NULL;
    if (ok && !entry) {
      size_t hidden = hidden_builds(identity.id, explicit_version ? identity.version : NULL);
      if (hidden && !client.provided_readable) {
        snprintf(client.message, sizeof(client.message),
            "%s has %zu build%s, but this system's ABI is unreadable (no system_info grant)",
            argv[i], hidden, hidden == 1 ? "" : "s");
        ok = fail(client.message);
      } else if (hidden) {
        snprintf(client.message, sizeof(client.message),
            "%s is not available for this system (%zu build%s for other systems)", argv[i], hidden,
            hidden == 1 ? "" : "s");
        ok = fail(client.message);
      } else {
        ok = fail("requested package version is not available");
      }
    }
    if (ok) {
      requested[i] = entry->entry.identity;
      for (int j = 0; j < i; ++j) {
        if (!strcmp(requested[i].id, requested[j].id)) {
          ok = fail("one package ID was requested more than once");
        }
      }
      ok = ok && select_version(selection, &requested[i], true);
    }
  }
  ok = ok && stale_check(selection, requested, (size_t)argc, "install", argc, argv);
  for (size_t i = 0; ok && i < selection->count; ++i) {
    bool explicit_request = false;
    for (int j = 0; j < argc; ++j) {
      explicit_request |= !strcmp(selection->entries[i].id, requested[j].id);
    }
    struct pkg_manifest manifest = {0};
    handle_t existing;
    bool present = pkg_tree_open(client.packages, &selection->entries[i], PKG_READ_RIGHTS,
        &existing, &client.reason);
    if (present) handle_close(existing);
    client.reason = NULL;
    if (explicit_request || i >= initial_count || !present) {
      const struct offered_package *entry = offering(selection->entries[i].id,
          selection->entries[i].version);
      ok = entry ? install_version(entry, &manifest) :
          fail("an exact dependency is unavailable in the configured repositories");
    } else {
      ok = trust_version(&selection->entries[i], &manifest);
    }
    for (size_t d = 0; ok && d < manifest.dependency_count; ++d) {
      ok = select_version(selection, &manifest.dependencies[d], false);
    }
    pkg_manifest_free(&manifest);
  }
  free(requested);
  return ok;
}

static bool prepare_remove(struct pkg_selection *selection, int argc, char **argv)
{
  for (int i = 0; i < argc; ++i) {
    if (!package_id_valid(argv[i])) return fail("invalid package ID");
    size_t index = find_package(selection, argv[i]);
    if (index == SIZE_MAX) return fail("package to remove is not installed");
    memmove(selection->entries + index, selection->entries + index + 1,
        (selection->count - index - 1) * sizeof(*selection->entries));
    --selection->count;
  }
  /* Every remaining member is carried over from the current set. */
  return stale_check(selection, NULL, 0, "remove", argc, argv) &&
      verify_selection(selection, selection);
}

static bool review(const struct pkg_selection *before, const struct pkg_selection *after)
{
  puts("Proposed package set (new launches only):");
  for (size_t i = 0; i < after->count; ++i) {
    size_t old = find_package(before, after->entries[i].id);
    if (old == SIZE_MAX || strcmp(before->entries[old].version, after->entries[i].version)) {
      printf("  %s %s@%s\n", old == SIZE_MAX ? "add" : "select",
          after->entries[i].id, after->entries[i].version);
    }
  }
  for (size_t i = 0; i < before->count; ++i) {
    if (find_package(after, before->entries[i].id) == SIZE_MAX) {
      printf("  remove %s@%s\n", before->entries[i].id, before->entries[i].version);
    }
  }
  if (client.terminal.input == HANDLE_INVALID || client.terminal.output == HANDLE_INVALID) {
    return fail("package changes require an interactive console review");
  }
  char line[16];
  struct term_line_result result = term_read_line(&client.terminal,
      "Switch to this package set? [y/N] ", line, sizeof(line));
  if (result.status != TERM_LINE_OK || result.limit_reached ||
      (strcmp(line, "y") && strcmp(line, "Y"))) {
    return fail("change cancelled; any prepared package trees remain inactive");
  }
  return true;
}

static void show_sets(void)
{
  handle_t sets;
  const char *reason = NULL;
  if (!pkg_directory(client.packages, "sets", false, PKG_READ_RIGHTS, &sets, &reason)) {
    return;
  }
  puts("Available set records (verified on explicit rollback):");
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply reply;
    if (directory_enumerate(sets, &cursor, name, sizeof(name), &reply) != CALL_OK ||
        reply.outcome != DIRECTORY_ENTRY) break;
    cursor = reply.cursor;
    size_t length = strlen(name);
    if (length > 4 && !strcmp(name + length - 4, ".lua")) {
      printf("  %.*s\n", (int)(length - 4), name);
    }
  }
  handle_close(sets);
}

static bool unlock(void)
{
  puts("pkg unlock: recovery only; the previous writer must already have stopped.");
  handle_t staging;
  enum call_status status = directory_lookup(client.packages, "staging",
      DIRECTORY_KIND_DIRECTORY, PKG_READ_RIGHTS, &staging);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    return fail("cannot inspect staging; lock retained");
  }
  puts("Staging contents (retained, not activated or deleted):");
  if (status == CALL_OK) {
    struct directory_cursor cursor = {0};
    for (;;) {
      char name[256];
      struct directory_enumerate_reply reply;
      status = directory_enumerate(staging, &cursor, name, sizeof(name), &reply);
      if (status != CALL_OK || (reply.outcome != DIRECTORY_END && reply.outcome != DIRECTORY_ENTRY)) {
        handle_close(staging);
        return fail("staging changed or cannot be read; lock retained");
      }
      if (reply.outcome == DIRECTORY_END) break;
      cursor = reply.cursor;
      printf("  %s (%s)\n", name, reply.kind == DIRECTORY_KIND_DIRECTORY ? "directory" : "file");
    }
    handle_close(staging);
  }
  status = directory_remove(client.packages, PKG_LOCK, DIRECTORY_KIND_DIRECTORY);
  if (status == CALL_NOT_FOUND) {
    puts("pkg: no writer lock");
    return true;
  }
  if (status != CALL_OK || directory_sync(client.packages) != CALL_OK) {
    return fail("writer lock removal could not be confirmed");
  }
  puts("pkg: abandoned lock cleared; staging retained");
  return true;
}

static bool package_volume(handle_t root, bool writable)
{
  struct handle_info info;
  struct directory_filesystem_info filesystem;
  uint64_t rights = PKG_READ_RIGHTS | DIRECTORY_RIGHT_FILESYSTEM_INFO;
  if (writable) rights |= PKG_WRITE_RIGHTS;
  if (root == HANDLE_INVALID || handle_query(root, &info) != CALL_OK ||
      info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_DIRECTORY ||
      info.transport || (rights & ~info.rights) ||
      directory_filesystem_info(root, &filesystem) != CALL_OK ||
      filesystem.type != FILESYSTEM_TYPE_NPFS ||
      (writable && (filesystem.flags & FILESYSTEM_FLAG_READ_ONLY))) {
    return fail(writable ? "writable npfs package management authority is unavailable" :
        "npfs package inspection authority is unavailable");
  }
  return true;
}

static bool mutate(const char *command, int argc, char **argv)
{
  if (!package_volume(client.management, true)) {
    return false;
  }
  client.packages = client.management;
  if (!strcmp(command, "unlock")) return unlock();
  handle_t lock;
  if (directory_create(client.packages, PKG_LOCK, DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_ENUMERATE, &lock) != CALL_OK) {
    return fail("writer lock unavailable; use pkg unlock only after the prior client has stopped");
  }
  bool uncertain = false;
  struct pkg_selection before = {0}, after = {0};
  bool current_valid = pkg_selection_load(client.packages, true, 0, &before, &client.reason);
  bool metadata_valid = current_valid;
  bool reindex = !strcmp(command, "reindex");
  if (metadata_valid && before.number && strcmp(command, "rollback") && !reindex) {
    uint64_t predecessor = 0;
    metadata_valid = pkg_predecessor(client.packages, before.number,
        &predecessor, &client.reason);
    if (metadata_valid && predecessor) {
      struct pkg_selection retained = {0};
      metadata_valid = pkg_selection_load(client.packages, false, predecessor,
          &retained, &client.reason);
      pkg_selection_free(&retained);
    }
  }
  bool ok = metadata_valid;
  uint64_t previous = before.number;
  struct pkg_projection projection = {0};
  if (reindex) {
    ok = ok && stale_check(&before, NULL, 0, "reindex", 0, NULL) && verify_selection(&before, NULL) &&
        pkg_commands_build(client.packages, &before, &projection, &client.reason) &&
        pkg_reindex(client.packages, before.number, &projection, &client.reason);
    if (ok) puts("pkg: active command index bootstrapped; selection unchanged");
  } else if (!strcmp(command, "rollback")) {
    uint64_t target = 0;
    if (argc) {
      for (const char *p = argv[0]; *p; ++p) {
        if (*p < '0' || *p > '9' || target > ((uint64_t)INT64_MAX - (*p - '0')) / 10) {
          target = 0;
          break;
        }
        target = target * 10 + *p - '0';
      }
      ok = target > 0;
      if (!ok) client.reason = "invalid explicit set number";
    } else {
      ok = current_valid && before.number &&
          pkg_predecessor(client.packages, before.number, &target, &client.reason);
      if (!ok) client.reason = "rollback needs explicit SET when the active predecessor is unavailable";
    }
    ok = ok && pkg_selection_load(client.packages, false, target, &after, &client.reason) &&
        stale_check(&after, NULL, 0, "rollback", 0, NULL) &&
        verify_selection(&after, current_valid ? &before : NULL);
    if (ok && !current_valid) {
      previous = target;
      puts("pkg: prior selection is unreadable; recovery target is retained as predecessor");
    }
  } else if (ok) {
    ok = pkg_selection_load(client.packages, true, 0, &after, &client.reason);
    if (ok && !strcmp(command, "install")) {
      ok = prepare_install(&after, argc, argv);
    } else if (ok) {
      ok = prepare_remove(&after, argc, argv);
    }
  }
  if (ok && !reindex) {
    ok = pkg_commands_build(client.packages, &after, &projection, &client.reason);
  }
  if (ok && !reindex) {
    ok = review(&before, &after);
    if (!ok && !client.reason) client.reason = "change cancelled";
  }
  if (ok && !reindex && console_passthrough(client.terminal.input, &client.passthrough) != CALL_OK) {
    ok = fail("cannot guard the confirmed set switch");
  }
  if (ok && !reindex) {
    ok = pkg_publish(client.packages, &after, previous, &projection, &uncertain,
        &client.reason);
  }
  /* Root records are the set's data packages. A space without the packages
   * source cannot see them, so say where they are readable. */
  for (size_t i = 0; ok && !reindex && i < projection.roots; ++i) {
    const struct package_command_record *root = &projection.records[projection.commands + i];
    if (find_package(&before, root->id) == SIZE_MAX) {
      printf("pkg: %s is read as %s:// in spaces that hold the packages source\n",
          root->id, root->id);
    }
  }
  free(projection.records);
  if (!ok && (!metadata_valid || !strcmp(command, "rollback"))) show_sets();
  pkg_selection_free(&after);
  pkg_selection_free(&before);
  handle_close(lock);
  if (!uncertain) ok = release_writer_lock(client.packages, ok);
  return ok;
}

static bool repository_available(const char *url)
{
  handle_t file;
  if (!fetch(url, "index.lua", true, 0, NULL, &file)) {
    return fail("not a package repository; repository configuration unchanged");
  }
  char *bytes = NULL;
  size_t size = 0, count = 0;
  struct pkg_entry *entries = NULL;
  bool ok = read_snapshot(file, &bytes, &size) &&
      pkg_index_parse(bytes, size, &entries, &count, &client.reason);
  free(bytes);
  free(entries);
  handle_close(file);
  if (!ok) {
    fprintf(stderr, "pkg: not a package repository: %sindex.lua: %s\n",
        url, client.reason ? client.reason : "index validation failed");
  }
  return ok;
}

static bool repository_command(const char *command, const char *url)
{
  if (!strcmp(command, "list")) {
    if (pkg_repositories_load(client.config, &client.repositories, &client.reason) != CALL_OK) {
      return false;
    }
    for (size_t i = 0; i < client.repositories.count; ++i) {
      puts(client.repositories.urls[i]);
    }
    if (!client.repositories.count) puts("No repositories; use pkg repository add URL.");
    return true;
  }
  char normalized[PKG_BASE_URL_MAX + 1];
  size_t length = strnlen(url, sizeof(normalized));
  if (!length || length > PKG_BASE_URL_MAX ||
      (url[length - 1] != '/' && length == PKG_BASE_URL_MAX)) {
    return fail("repository URL must fit 2048 bytes including its final /");
  }
  memcpy(normalized, url, length);
  if (normalized[length - 1] != '/') {
    normalized[length++] = '/';
  }
  normalized[length] = 0;
  url = normalized;
  if (!pkg_repository_url_valid(url)) {
    return fail("repository URL must be HTTP/HTTPS without credentials, query or fragment; final / is optional");
  }
  if (!package_volume(client.management, true)) return false;
  if (client.config_write == HANDLE_INVALID) {
    return fail("confined package_config_write authority is unavailable");
  }
  handle_t lock;
  if (directory_create(client.management, PKG_LOCK, DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_ENUMERATE, &lock) != CALL_OK) {
    return fail("writer lock unavailable; use pkg unlock only after the prior client has stopped");
  }
  bool uncertain = false;
  bool ok = pkg_repositories_load(client.config_write, &client.repositories,
      &client.reason) == CALL_OK;
  size_t index = 0;
  while (ok && index < client.repositories.count &&
      strcmp(client.repositories.urls[index], url)) ++index;
  bool changed = false;
  if (ok && !strcmp(command, "add")) {
    if (index < client.repositories.count) {
      ok = fail("repository already configured");
    } else if (client.repositories.count == PKG_REPOSITORIES_MAX) {
      ok = fail("at most eight repositories may be configured");
    } else if (repository_available(url)) {
      strcpy(client.repositories.urls[client.repositories.count++], url);
      changed = true;
    } else {
      ok = false;
    }
  } else if (ok) {
    if (index == client.repositories.count) {
      ok = fail("repository not found");
    } else {
      memmove(client.repositories.urls + index, client.repositories.urls + index + 1,
          (client.repositories.count - index - 1) * sizeof(client.repositories.urls[0]));
      --client.repositories.count;
      changed = true;
    }
  }
  if (ok && changed) {
    enum call_status status = pkg_repositories_stage(client.config_write,
        &client.repositories, &client.reason);
    uncertain = status == CALL_OUTCOME_UNKNOWN;
    ok = status == CALL_OK;
    if (uncertain) {
      fail("repository preparation uncertain; writer lock retained for explicit recovery");
    }
  }
  /* Keep ordinary interruption during preparation; protect only publication.
   * Non-console callers need no foreground keyboard guard. */
  if (ok && changed && client.terminal.input != HANDLE_INVALID &&
      console_passthrough(client.terminal.input, &client.passthrough) != CALL_OK) {
    ok = fail("cannot guard the repository configuration switch");
  }
  if (ok && changed) {
    uncertain = true;
    enum call_status status = directory_rename(client.config_write, "packages.lua.new",
        client.config_write, "packages.lua", DIRECTORY_RENAME_REPLACE, DIRECTORY_KIND_FILE);
    if (status != CALL_OK || directory_sync(client.config_write) != CALL_OK) {
      ok = fail("repository configuration uncertain; inspect packages.lua before explicit pkg unlock; no retry was made");
    } else {
      uncertain = false;
      puts("pkg: repository configuration updated");
    }
  }
  handle_close(lock);
  if (!uncertain) ok = release_writer_lock(client.management, ok);
  return ok;
}

static int usage(void)
{
  fputs("usage: pkg [--timeout SECONDS] status | list [--available] | install ID[@VERSION]... | remove ID... | rollback [SET] | verify | reindex | unlock | repository list | repository add URL | repository remove URL\nRepository URLs accept an optional final / (2048 bytes including the stored final /).\n", stderr);
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  client.clock = bootstrap_capability("clock");
  client.management = bootstrap_capability(PACKAGE_MANAGEMENT_SOURCE);
  client.packages = bootstrap_capability(PACKAGE_INSPECT_SOURCE);
  client.config = bootstrap_capability(PACKAGE_CONFIG_SOURCE);
  client.config_write = bootstrap_capability(PACKAGE_CONFIG_WRITE_SOURCE);
  client.system_info = bootstrap_capability("system_info");
  client.fetch = (struct pkg_fetch_authority){bootstrap_capability("http"),
    bootstrap_capability("https"), client.clock};
  struct pyxis_stdio_binding input = bootstrap_stdio(ENVIRONMENT_STDIN);
  struct pyxis_stdio_binding output = bootstrap_stdio(ENVIRONMENT_STDOUT);
  client.terminal = (struct terminal){input.protocol == PROTOCOL_CONSOLE ? input.handle : HANDLE_INVALID,
    output.protocol == PROTOCOL_CONSOLE ? output.handle : HANDLE_INVALID};
  int arg = 1;
  if (argc > 2 && !strcmp(argv[arg], "--timeout")) {
    if (pkg_timeout_parse(argv[arg + 1], &client.timeout) != CALL_OK) return usage();
    arg += 2;
  }
  if (arg >= argc) return usage();
  const char *command = argv[arg++];
  int count = argc - arg;
  char **arguments = argv + arg;
  bool repository = !strcmp(command, "repository");
  if (repository && (count < 1 ||
      (!strcmp(arguments[0], "list") ? count != 1 :
      (!strcmp(arguments[0], "add") || !strcmp(arguments[0], "remove")) ? count != 2 : true))) {
    return usage();
  }
  bool mutation = !strcmp(command, "install") || !strcmp(command, "remove") ||
      !strcmp(command, "rollback") || !strcmp(command, "reindex") || !strcmp(command, "unlock");
  if (!repository && ((!strcmp(command, "install") || !strcmp(command, "remove")) ? count < 1 :
      !strcmp(command, "rollback") ? count > 1 :
      !strcmp(command, "list") ? count > 1 || (count && strcmp(arguments[0], "--available")) : count != 0)) {
    return usage();
  }
  bool needs_hash = (mutation && strcmp(command, "unlock")) || !strcmp(command, "verify");
  bool hash_ready = false;
  if (needs_hash) {
    hash_ready = hash_init(client.clock, bootstrap_capability("random"));
    if (!hash_ready) {
      fputs("pkg: SHA-256 initialization unavailable\n", stderr);
      return EXIT_FAILURE;
    }
  }
  bool ok = false;
  if (repository) {
    ok = repository_command(arguments[0], count > 1 ? arguments[1] : NULL);
  } else if (mutation) {
    ok = mutate(command, count, arguments);
  } else if (!strcmp(command, "list") && count) {
    ok = load_repositories();
    for (size_t i = 0; ok && i < client.offered_count; ++i) {
      const struct pkg_entry *entry = &client.offered[i].entry;
      printf("%s@%s %s%s%s %llu bytes\n", entry->identity.id, entry->identity.version,
          entry->kind, entry->abi[0] ? " abi " : "", entry->abi,
          (unsigned long long)entry->size);
    }
    if (ok && client.hidden_count) {
      printf(client.provided_readable ? "%zu builds for other systems hidden\n" :
          "%zu builds hidden: this system's ABI is unreadable (no system_info grant)\n",
          client.hidden_count);
    }
  } else if (!strcmp(command, "status") || !strcmp(command, "list") || !strcmp(command, "verify")) {
    struct pkg_selection selection = {0};
    ok = package_volume(client.packages, false) &&
        pkg_selection_load(client.packages, true, 0, &selection, &client.reason);
    if (ok) {
      print_selection(&selection);
      if (!strcmp(command, "verify")) {
        ok = stale_check(&selection, NULL, 0, "verify", 0, NULL) && verify_selection(&selection, NULL);
        struct pkg_projection projection = {0};
        ok = ok && pkg_commands_build(client.packages, &selection, &projection,
            &client.reason) &&
            pkg_commands_verify(client.packages, selection.number, &projection, &client.reason);
        free(projection.records);
        if (ok) puts("pkg: selected package files and command index verified");
      }
    } else {
      show_sets();
    }
    pkg_selection_free(&selection);
  } else {
    release_publish_guard();
    if (hash_ready) hash_close();
    free(client.offered);
    free(client.hidden);
    return usage();
  }
  release_publish_guard();
  if (hash_ready) hash_close();
  free(client.offered);
  free(client.hidden);
  if (!ok) {
    if (client.reason || client.cleanup_status == CALL_OK) {
      fprintf(stderr, "pkg: %s\n", client.reason ? client.reason : "operation failed");
    }
    if (client.cleanup_status != CALL_OK) {
      fprintf(stderr, "pkg: writer lock cleanup unconfirmed (status %u); inspect with pkg unlock\n",
          client.cleanup_status);
    }
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
