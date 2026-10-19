#include "archive.h"
#include "fetch.h"
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
  handle_t packages, clock, config, management;
  struct terminal terminal;
  handle_t passthrough;
  struct pkg_fetch_authority fetch;
  struct pkg_repositories repositories;
  struct offered_package *offered;
  size_t offered_count;
  uint64_t timeout;
  bool cancelled;
  char detail[RESOURCE_DETAIL_MAX_BYTES + 1];
  const char *reason;
};

/* The literal parser imposes the stricter eventual set-record bound. */
#define PKG_SELECTION_MAX 8192

static struct client client;

/* While waiting inside the HTTP provider, preserve normal foreground Ctrl+C.
 * Local verification/extraction polls Ctrl+C as data at bounded I/O boundaries. */
static void passthrough(bool enabled)
{
  if (!enabled && client.passthrough != HANDLE_INVALID) {
    handle_close(client.passthrough);
    client.passthrough = HANDLE_INVALID;
  }
  if (enabled && client.terminal.input != HANDLE_INVALID &&
      client.passthrough == HANDLE_INVALID) {
    console_passthrough(client.terminal.input, &client.passthrough);
  }
}

bool pkg_cancelled(void)
{
  if (!client.cancelled && client.passthrough != HANDLE_INVALID) {
    unsigned char bytes[32];
    size_t count = 0;
    enum call_status status = console_read_timeout(client.terminal.input, bytes,
        sizeof(bytes), 0, &count);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      client.cancelled = true;
    }
    for (size_t i = 0; i < count; ++i) {
      client.cancelled |= bytes[i] == 3;
    }
  }
  return client.cancelled;
}

static bool fail(const char *reason)
{
  client.reason = reason;
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
    if (pkg_cancelled() || file_read(file, offset, buffer + offset,
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

static bool fetch(const char *base, const char *relative, bool index, uint64_t size,
    handle_t *file)
{
  passthrough(false);
  enum call_status status = pkg_fetch(&client.fetch, base, relative, index, size,
      client.timeout, file, client.detail);
  passthrough(true);
  if (status != CALL_OK) {
    fprintf(stderr, "pkg: fetch failed (status %u): %s\n", status, client.detail);
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
    return fail("no repositories configured in system://config/packages.lua");
  }
  for (size_t r = 0; r < client.repositories.count; ++r) {
    handle_t file;
    printf("pkg: reading repository %zu/%zu\n", r + 1, client.repositories.count);
    if (!fetch(client.repositories.urls[r], "index.lua", true, 0, &file)) {
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
  printf("pkg: verifying %s@%s\n", identity->id, identity->version);
  bool ok = pkg_tree_verify(tree, NULL, manifest, &client.reason);
  handle_close(tree);
  if (ok && (strcmp(manifest->identity.id, identity->id) ||
      strcmp(manifest->identity.version, identity->version))) {
    pkg_manifest_free(manifest);
    return fail("stored manifest does not match its package directory");
  }
  return ok;
}

static bool install_version(const struct offered_package *offered, struct pkg_manifest *manifest)
{
  const struct pkg_identity *identity = &offered->entry.identity;
  printf("pkg: fetching %s@%s (%llu bytes)\n", identity->id, identity->version,
      (unsigned long long)offered->entry.size);
  handle_t snapshot;
  if (!fetch(client.repositories.urls[offered->repository], offered->entry.archive,
        false, offered->entry.size, &snapshot)) {
    return false;
  }
  handle_t existing = HANDLE_INVALID;
  bool exists = pkg_tree_open(client.packages, identity, PKG_READ_RIGHTS,
      &existing, &client.reason);
  client.reason = NULL;
  if (exists) {
    struct pkg_manifest desired = {0};
    bool ok = pkg_archive(snapshot, &offered->entry, HANDLE_INVALID, &desired, &client.reason) &&
        pkg_tree_verify(existing, &desired, manifest, &client.reason);
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
  ok = ok && pkg_archive(snapshot, &offered->entry, destination, manifest, &client.reason);
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

static bool verify_selection(const struct pkg_selection *selection)
{
  for (size_t i = 0; i < selection->count; ++i) {
    struct pkg_manifest manifest = {0};
    if (!verify_version(&selection->entries[i], &manifest)) {
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
      ok = fail("requested package version is not available");
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
    if (explicit_request || !present) {
      const struct offered_package *entry = offering(selection->entries[i].id,
          selection->entries[i].version);
      ok = entry ? install_version(entry, &manifest) :
          fail("an exact dependency is unavailable in the configured repositories");
    } else {
      ok = verify_version(&selection->entries[i], &manifest);
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
  return verify_selection(selection);
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
      "Type apply to switch, or cancel: ", line, sizeof(line));
  if (result.status != TERM_LINE_OK || result.limit_reached || strcmp(line, "apply")) {
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
  bool ok = current_valid;
  uint64_t previous = before.number;
  if (!strcmp(command, "rollback")) {
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
        verify_selection(&after);
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
  if (ok) {
    ok = review(&before, &after) && !pkg_cancelled();
    if (!ok && !client.reason) client.reason = "change cancelled";
  }
  if (ok) {
    ok = pkg_publish(client.packages, &after, previous, &uncertain, &client.reason);
  }
  if (!ok && !current_valid) show_sets();
  pkg_selection_free(&after);
  pkg_selection_free(&before);
  handle_close(lock);
  if (!uncertain) {
    enum call_status status = directory_remove(client.packages, PKG_LOCK, DIRECTORY_KIND_DIRECTORY);
    if (status != CALL_OK || directory_sync(client.packages) != CALL_OK) {
      ok = fail("writer lock cleanup unconfirmed; inspect with pkg unlock");
    }
  }
  return ok;
}

static int usage(void)
{
  fputs("usage: pkg [--timeout SECONDS] status | list [--available] | install ID[@VERSION]... | remove ID... | rollback [SET] | verify | unlock\n", stderr);
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  client.clock = bootstrap_capability("clock");
  client.management = bootstrap_capability(PACKAGE_MANAGEMENT_SOURCE);
  client.packages = bootstrap_capability(PACKAGE_INSPECT_SOURCE);
  client.config = bootstrap_capability(PACKAGE_CONFIG_SOURCE);
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
  bool mutation = !strcmp(command, "install") || !strcmp(command, "remove") ||
      !strcmp(command, "rollback") || !strcmp(command, "unlock");
  if ((!strcmp(command, "install") || !strcmp(command, "remove")) ? count < 1 :
      !strcmp(command, "rollback") ? count > 1 :
      !strcmp(command, "list") ? count > 1 || (count && strcmp(arguments[0], "--available")) : count != 0) {
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
  passthrough(true);
  bool ok = false;
  if (mutation) {
    ok = mutate(command, count, arguments);
  } else if (!strcmp(command, "list") && count) {
    ok = load_repositories();
    for (size_t i = 0; ok && i < client.offered_count; ++i) {
      printf("%s@%s %s %llu bytes\n", client.offered[i].entry.identity.id,
          client.offered[i].entry.identity.version, client.offered[i].entry.kind,
          (unsigned long long)client.offered[i].entry.size);
    }
  } else if (!strcmp(command, "status") || !strcmp(command, "list") || !strcmp(command, "verify")) {
    struct pkg_selection selection = {0};
    ok = package_volume(client.packages, false) &&
        pkg_selection_load(client.packages, true, 0, &selection, &client.reason);
    if (ok) {
      print_selection(&selection);
      if (!strcmp(command, "verify")) {
        ok = verify_selection(&selection);
        if (ok) puts("pkg: all selected package files verified");
      }
    } else {
      show_sets();
    }
    pkg_selection_free(&selection);
  } else {
    passthrough(false);
    if (hash_ready) hash_close();
    free(client.offered);
    return usage();
  }
  passthrough(false);
  if (hash_ready) hash_close();
  free(client.offered);
  if (!ok) fprintf(stderr, "pkg: %s\n", client.reason ? client.reason : "operation failed");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
