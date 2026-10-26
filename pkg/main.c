#include "archive.h"
#include "commands.h"
#include "sdk.h"
#include "fetch.h"
#include "progress.h"
#include "signature.h"
#include "store.h"
#include "upgrade.h"
#include "../common/bootstrap_environment.h"
#include "../xfer/hash.h"
#include <clock.h>
#include <console.h>
#include <file.h>
#include <handle.h>
#include <literal.h>
#include <path.h>
#include <package_maintenance.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>

struct offered_package {
  struct pkg_entry entry;
  size_t repository;
};

struct hidden_build {
  struct pkg_identity identity;
  size_t repository;
};

/* One install argument: [REPO/]ID[@VERSION]. */
struct package_request {
  struct pkg_identity identity;
  bool explicit_version;
  size_t repository; /* ANY_REPOSITORY unless REPO/ names one. */
};

#define ANY_REPOSITORY SIZE_MAX
/* The configured repository pkg upgrade reads releases from (decision 74);
 * other commands do not read it. */
#define RELEASES_REPOSITORY "releases"
#define RELEASE_ID "pyxis"
#define UPGRADE_STAGING ".upgrade"

struct client {
  handle_t packages, clock, config, config_write, management;
  /* What the running system provides, read once on first use. */
  struct package_provided_abi provided;
  bool provided_loaded, provided_readable;
  /* Builds for an ABI this system does not provide, or for any ABI when the
   * running one is unreadable: not offered, only reported. */
  struct hidden_build *hidden;
  size_t hidden_count;
  char message[2048];
  struct terminal terminal;
  handle_t passthrough;
  struct pkg_fetch_authority fetch;
  struct pkg_repositories repositories;
  struct offered_package *offered;
  size_t offered_count;
  /* The offers this operation fetches, for the review's origins. */
  const struct offered_package **fetched;
  size_t fetched_count;
  uint64_t timeout;
  /* --yes: switch without the console question, for Install. */
  bool yes;
  bool crypto_ready; /* PSA signature crypto initialized; closed on exit. */
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
    client.provided_readable = package_provided_abi(&client.provided) == CALL_OK;
  }
  return &client.provided;
}

/* A program, toolchain or dev package is usable only on a system that provides its
 * ABI. Refusals name both sides; INSTALLED adds the way out for a stored one. */
static bool abi_check(const char *id, const char *version, const char *kind,
    const char *abi, bool installed)
{
  if (!pkg_kind_bound(kind)) {
    return true;
  }
  const struct package_provided_abi *provided = provided_abi();
  char remedy[2 * PACKAGE_ID_MAX + 80] = "";
  if (installed) {
    snprintf(remedy, sizeof(remedy),
        "; pkg install %s takes a build for this system, pkg remove %s drops it", id, id);
  }
  if (!client.provided_readable) {
    snprintf(client.message, sizeof(client.message),
        "%s %s needs an ABI check, but the running system's ABI is unreadable (%s)",
        id, version, PACKAGE_OS_RELEASE);
  } else if (!package_abi_provided(provided, abi)) {
    snprintf(client.message, sizeof(client.message),
        "%s %s needs ABI %s; this system provides %s%s", id, version, abi, provided->text,
        remedy);
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
        "cannot check the ABI of %s: this system's ABI is unreadable (%s)", stale,
        PACKAGE_OS_RELEASE);
    return fail(client.message);
  }
  char install[1100] = "", remove[1100] = "";
  bool installing = !strcmp(command, "install"), removing = !strcmp(command, "remove");
  for (int i = 0; (installing || removing) && i < argc; ++i) {
    append(installing ? install : remove, sizeof(install), " ", argv[i]);
  }
  append(install, sizeof(install), " ", stale);
  append(remove, sizeof(remove), " ", stale);
  snprintf(client.message, sizeof(client.message),
      "%zu installed package%s not built for this system, which provides %s: %s. "
      "Replace with: pkg install %s. Or drop with: pkg remove %s",
      count, count == 1 ? " is" : "s are", provided->text, details, install, remove);
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

/* A dependency without a version (empty) keeps whatever version the selection
 * holds; when absent, its version is chosen when it is fetched. */
static bool select_version(struct pkg_selection *selection,
    const struct pkg_identity *identity, bool replace)
{
  size_t index = find_package(selection, identity->id);
  if (index != SIZE_MAX) {
    if (!replace && !identity->version[0]) {
      return true;
    }
    if (!replace && selection->entries[index].version[0] &&
        strcmp(selection->entries[index].version, identity->version)) {
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
  /* NUL-terminated for the PEM parser; size excludes the terminator. */
  char *buffer = malloc(length + 1);
  if (!buffer) {
    return fail("cannot allocate repository index");
  }
  buffer[length] = 0;
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

static bool ensure_crypto(void)
{
  if (!client.crypto_ready) {
    client.crypto_ready = pkg_crypto_init(client.clock, bootstrap_capability("random"));
  }
  return client.crypto_ready || fail("signature crypto initialization unavailable");
}

static bool fetch_bytes(const char *base, const char *relative, char **bytes, size_t *size)
{
  handle_t file;
  if (!fetch(base, relative, true, 0, NULL, &file)) {
    return false;
  }
  bool ok = read_snapshot(file, bytes, size);
  handle_close(file);
  return ok;
}

/* What this packages volume last accepted from a signed repository
 * (decision 71): repositories/NAME holds the URL, serial, signed time and the
 * index's SHA-256, one per line. Another URL under the same name starts over. */
struct accepted_index {
  char url[PKG_BASE_URL_MAX + 1];
  uint64_t serial;
  char signed_at[PKG_TIME_LENGTH + 1];
  char sha256[65];
};

#define ACCEPTED_DIRECTORY "repositories"
#define ACCEPTED_MAX_BYTES (PKG_BASE_URL_MAX + 128)

static bool accepted_read(const struct pkg_repository *repository, struct accepted_index *accepted)
{
  *accepted = (struct accepted_index){0};
  handle_t directory;
  const char *reason = NULL;
  if (client.packages == HANDLE_INVALID ||
      directory_lookup(client.packages, ACCEPTED_DIRECTORY, DIRECTORY_KIND_DIRECTORY,
          PKG_READ_RIGHTS, &directory) != CALL_OK) {
    return false;
  }
  char *bytes = NULL;
  size_t size = 0;
  bool read = pkg_text(directory, repository->name, ACCEPTED_MAX_BYTES, &bytes, &size, &reason);
  handle_close(directory);
  if (!read) {
    return false;
  }
  char serial[24];
  bool ok = sscanf(bytes, "%2048[^\n]\n%23[0-9]\n%20[0-9TZ:-]\n%64[0-9a-f]\n", accepted->url,
      serial, accepted->signed_at, accepted->sha256) == 4 &&
      !strcmp(accepted->url, repository->url) && pkg_time_valid(accepted->signed_at) &&
      strlen(accepted->sha256) == 64;
  accepted->serial = ok ? strtoull(serial, NULL, 10) : 0;
  free(bytes);
  if (!ok) {
    *accepted = (struct accepted_index){0};
  }
  return ok;
}

/* Only a writer under the lock records what it accepted. */
static bool accepted_write(const struct pkg_repository *repository,
    const struct accepted_index *accepted)
{
  handle_t directory;
  if (!pkg_directory(client.packages, ACCEPTED_DIRECTORY, true, PKG_WRITE_RIGHTS, &directory,
      &client.reason)) {
    return false;
  }
  char text[ACCEPTED_MAX_BYTES];
  int length = snprintf(text, sizeof(text), "%s\n%llu\n%s\n%s\n", accepted->url,
      (unsigned long long)accepted->serial, accepted->signed_at, accepted->sha256);
  char staged[PACKAGE_ID_MAX + 8];
  snprintf(staged, sizeof(staged), "%s.new", repository->name);
  enum call_status removed = directory_remove(directory, staged, DIRECTORY_KIND_FILE);
  bool ok = (removed == CALL_OK || removed == CALL_NOT_FOUND) &&
      pkg_write_text(directory, staged, text, (size_t)length, &client.reason) &&
      directory_rename(directory, staged, directory, repository->name,
          DIRECTORY_RENAME_REPLACE, DIRECTORY_KIND_FILE) == CALL_OK &&
      directory_sync(directory) == CALL_OK;
  handle_close(directory);
  return ok || fail("cannot record the accepted repository index");
}

/* Reads REPOSITORY's published index (docs/wip/pkg-upgrade.md, decisions 71
 * and 72): current, the index/N.lua it names and, unless the entry says
 * signed = false, that index's signature and signer, checked against the pinned
 * CA. A signed index with a lower serial or an earlier signed time than the one
 * this packages volume last accepted, or signed before this image's
 * signed_floor, is refused; a writer records the one it accepts. Success owns
 * *entries. */
static bool read_repository(const struct pkg_repository *repository,
    struct pkg_entry **entries, size_t *count)
{
  *entries = NULL;
  *count = 0;
  char *current = NULL, *bytes = NULL, *signature = NULL, *signer = NULL, *anchor = NULL;
  size_t current_size = 0, size = 0, signature_size = 0, signer_size = 0;
  struct pkg_index_header header;
  char path[64];
  uint64_t serial = 0;
  bool ok = fetch_bytes(repository->url, "current", &current, &current_size);
  if (ok) {
    char *end = NULL;
    serial = current_size && current[0] >= '1' && current[0] <= '9' ?
        strtoull(current, &end, 10) : 0;
    ok = serial && serial <= PKG_SERIAL_MAX && end == current + current_size - 1 && *end == '\n';
    if (!ok) fail("the repository's current is not one serial");
  }
  if (ok) {
    snprintf(path, sizeof(path), "index/%llu.lua", (unsigned long long)serial);
    ok = fetch_bytes(repository->url, path, &bytes, &size) &&
        pkg_index_parse(bytes, size, &header, entries, count, &client.reason);
  }
  if (ok && header.serial != serial) {
    ok = fail("the published index names another serial than current");
  }
  if (!ok || repository->unsigned_index) {
    goto done;
  }
  if (strcmp(header.repository, repository->name)) {
    snprintf(client.message, sizeof(client.message),
        "the index was published as repository %s, not %s", header.repository, repository->name);
    ok = fail(client.message);
  } else if (!header.signer[0]) {
    ok = fail("the index is unsigned; only an entry with signed = false accepts that");
  }
  char signature_path[80];
  snprintf(signature_path, sizeof(signature_path), "%s.sig", path);
  FILE *pinned = NULL;
  ok = ok && ensure_crypto() &&
      fetch_bytes(repository->url, signature_path, &signature, &signature_size) &&
      fetch_bytes(repository->url, header.signer, &signer, &signer_size);
  if (ok) {
    pinned = fopen(PKG_SIGNING_ANCHOR, "rb");
    anchor = pinned ? malloc(8192) : NULL;
    size_t anchor_size = anchor ? fread(anchor, 1, 8191, pinned) : 0;
    if (pinned) fclose(pinned);
    ok = anchor && anchor_size && anchor_size < 8191;
    if (!ok) {
      fail("the pinned signing CA (" PKG_SIGNING_ANCHOR ") is unreadable");
    } else {
      anchor[anchor_size] = 0;
      ok = pkg_signature_verify(anchor, anchor_size + 1, signer, signer_size + 1,
          (const uint8_t *)bytes, size, (const uint8_t *)signature, signature_size,
          header.signed_at, &client.reason);
    }
  }
  struct accepted_index accepted = {0}, latest = {0};
  if (ok) {
    ok = pkg_sha256_text(bytes, size, latest.sha256) || fail("cannot hash the repository index");
  }
  if (ok) {
    strcpy(latest.url, repository->url);
    latest.serial = header.serial;
    strcpy(latest.signed_at, header.signed_at);
    const struct package_provided_abi *provided = provided_abi();
    bool known = accepted_read(repository, &accepted);
    if (known && header.serial < accepted.serial) {
      snprintf(client.message, sizeof(client.message),
          "index serial %llu is older than the accepted %llu; refusing a rollback",
          (unsigned long long)header.serial, (unsigned long long)accepted.serial);
      ok = fail(client.message);
    } else if (known && header.serial == accepted.serial && strcmp(latest.sha256, accepted.sha256)) {
      ok = fail("the index differs from the accepted one with the same serial");
    } else if (known && strcmp(header.signed_at, accepted.signed_at) < 0) {
      ok = fail("the index was signed before the accepted one; refusing it");
    } else if (!known && !client.provided_readable) {
      ok = fail("cannot read boot://os-release for the first-contact signing floor");
    } else if (!known && provided->signed_floor[0] &&
        strcmp(header.signed_at, provided->signed_floor) < 0) {
      ok = fail("the index was signed before this system was built; refusing it");
    }
  }
  if (ok && client.management != HANDLE_INVALID && client.packages == client.management &&
      (accepted.serial != latest.serial || strcmp(accepted.sha256, latest.sha256))) {
    ok = accepted_write(repository, &latest);
  }

done:
  free(current);
  free(bytes);
  free(signature);
  free(signer);
  free(anchor);
  if (!ok) {
    free(*entries);
    *entries = NULL;
    *count = 0;
  }
  return ok;
}

static bool load_repositories(void)
{
  if (pkg_repositories_load(client.config, &client.repositories, &client.reason) != CALL_OK) {
    return false;
  }
  if (!client.repositories.count) {
    return fail("no repositories configured; use pkg repository add NAME URL (HTTP/HTTPS base; final / optional)");
  }
  for (size_t r = 0; r < client.repositories.count; ++r) {
    const struct pkg_repository *repository = &client.repositories.entries[r];
    if (!strcmp(repository->name, RELEASES_REPOSITORY)) {
      continue;
    }
    printf("pkg: reading repository %s%s\n", repository->name,
        repository->unsigned_index ? " (unsigned)" : "");
    struct pkg_entry *entries = NULL;
    size_t count = 0;
    if (!read_repository(repository, &entries, &count)) {
      if (client.reason) {
        snprintf(client.detail, sizeof(client.detail), "repository %s: %s",
            repository->name, client.reason);
        client.reason = client.detail;
      }
      return false;
    }
    const struct package_provided_abi *provided = provided_abi();
    size_t usable = 0;
    for (size_t i = 0; i < count; ++i) {
      if (pkg_kind_release(entries[i].kind)) {
        continue;
      }
      if (pkg_kind_bound(entries[i].kind) && (!client.provided_readable ||
          !package_abi_provided(provided, entries[i].abi))) {
        struct hidden_build *hidden = realloc(client.hidden,
            (client.hidden_count + 1) * sizeof(*hidden));
        if (!hidden) {
          free(entries);
          return fail("cannot allocate repository catalog");
        }
        client.hidden = hidden;
        client.hidden[client.hidden_count++] = (struct hidden_build){entries[i].identity, r};
        continue;
      }
      entries[usable++] = entries[i];
    }
    count = usable;
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

/* The build of ID, at VERSION when given, that repository R offers for this
 * system: the last one its index lists. */
static const struct offered_package *offering(const char *id, const char *version, size_t r)
{
  const struct offered_package *found = NULL;
  for (size_t i = 0; i < client.offered_count; ++i) {
    const struct pkg_identity *identity = &client.offered[i].entry.identity;
    if (client.offered[i].repository == r && !strcmp(identity->id, id) &&
        (!version || !strcmp(identity->version, version))) {
      found = &client.offered[i];
    }
  }
  return found;
}

static size_t hidden_builds(const char *id, const char *version, size_t repository)
{
  size_t count = 0;
  for (size_t i = 0; i < client.hidden_count; ++i) {
    const struct pkg_identity *identity = &client.hidden[i].identity;
    count += (repository == ANY_REPOSITORY || client.hidden[i].repository == repository) &&
        !strcmp(identity->id, id) && (!version || !strcmp(identity->version, version));
  }
  return count;
}

static bool record_fetch(const struct offered_package *offered)
{
  const struct offered_package **fetched = realloc(client.fetched,
      (client.fetched_count + 1) * sizeof(*fetched));
  if (!fetched) {
    return fail("cannot allocate package origins");
  }
  client.fetched = fetched;
  client.fetched[client.fetched_count++] = offered;
  return true;
}

/* Picks the build of ID (at VERSION when given) from REPOSITORY, or from any
 * repository that offers one for this system. Several repositories give a
 * numbered choice on a console; elsewhere pkg refuses and lists the REPO/ID
 * spellings. A DEPENDENCY is only reached after the packages needing it were
 * fetched, so its cancellation says those stay inactive. *CHOSEN is NULL when
 * nothing is offered. */
static bool resolve(const char *id, const char *version, size_t repository, bool dependency,
    const struct offered_package **chosen)
{
  *chosen = NULL;
  const struct offered_package *candidates[PKG_REPOSITORIES_MAX];
  size_t count = 0;
  for (size_t r = 0; r < client.repositories.count; ++r) {
    const struct offered_package *found = repository == ANY_REPOSITORY || repository == r ?
        offering(id, version, r) : NULL;
    if (found) {
      candidates[count++] = found;
    }
  }
  if (count <= 1) {
    *chosen = count ? candidates[0] : NULL;
    return true;
  }
  printf("pkg: %s%s%s%s is offered by %zu repositories\n", dependency ? "dependency " : "",
      id, version ? "@" : "", version ? version : "", count);
  for (size_t i = 0; i < count; ++i) {
    const struct pkg_identity *identity = &candidates[i]->entry.identity;
    printf("  %zu  %s/%s@%s\n", i + 1,
        client.repositories.entries[candidates[i]->repository].name,
        identity->id, identity->version);
  }
  if (client.terminal.input == HANDLE_INVALID || client.terminal.output == HANDLE_INVALID) {
    snprintf(client.message, sizeof(client.message),
        "%s is offered by %zu repositories; name one as REPO/%s%s%s", id, count, id,
        version ? "@" : "", version ? version : "");
    return fail(client.message);
  }
  char prompt[48], line[16];
  snprintf(prompt, sizeof(prompt), "Choose 1-%zu, or press Enter to cancel: ", count);
  struct term_line_result result = term_read_line(&client.terminal, prompt, line, sizeof(line));
  char *end = line;
  unsigned long choice = result.status == TERM_LINE_OK && !result.limit_reached ?
      strtoul(line, &end, 10) : 0;
  if (end != line && !*end && choice >= 1 && choice <= count) {
    *chosen = candidates[choice - 1];
  }
  if (*chosen) {
    return true;
  }
  if (dependency) {
    snprintf(client.message, sizeof(client.message),
        "no repository chosen for dependency %s; packages fetched so far stay inactive and the active set is unchanged",
        id);
    return fail(client.message);
  }
  return fail("no repository chosen; nothing was fetched or changed");
}

static bool request(const char *text, struct package_request *request)
{
  *request = (struct package_request){.repository = ANY_REPOSITORY};
  const char *slash = strchr(text, '/');
  if (slash) {
    size_t length = (size_t)(slash - text);
    char name[PACKAGE_ID_MAX + 1];
    if (!length || length > PACKAGE_ID_MAX) {
      return fail("invalid repository name");
    }
    memcpy(name, text, length);
    name[length] = 0;
    for (size_t r = 0; r < client.repositories.count; ++r) {
      if (!strcmp(client.repositories.entries[r].name, name)) {
        request->repository = r;
      }
    }
    if (request->repository == ANY_REPOSITORY) {
      snprintf(client.message, sizeof(client.message),
          "no repository named %s; pkg repository list shows them", name);
      return fail(client.message);
    }
    text = slash + 1;
  }
  const char *at = strchr(text, '@');
  size_t length = at ? (size_t)(at - text) : strlen(text);
  if (!length || length > PACKAGE_ID_MAX) {
    return fail("invalid package ID");
  }
  memcpy(request->identity.id, text, length);
  request->identity.id[length] = 0;
  if (!package_id_valid(request->identity.id) || (at && !pkg_version_valid(at + 1))) {
    return fail("invalid package ID or version");
  }
  request->explicit_version = at != NULL;
  if (at) {
    strcpy(request->identity.version, at + 1);
  }
  return true;
}

/* check: reads only the repositories' indexes, with no packages volume, and
 * prints the build of TEXT this system can install. Install runs it before its
 * Review (docs/wip/pkg-upgrade.md, decision 67). */
static bool check(const char *text)
{
  struct package_request wanted;
  if (!load_repositories() || !request(text, &wanted)) {
    return false;
  }
  const char *version = wanted.explicit_version ? wanted.identity.version : NULL;
  const struct offered_package *chosen;
  if (!resolve(wanted.identity.id, version, wanted.repository, false, &chosen)) {
    return false;
  }
  if (chosen) {
    const struct pkg_entry *entry = &chosen->entry;
    printf("pkg: %s/%s@%s is offered for this system%s%s\n",
        client.repositories.entries[chosen->repository].name,
        entry->identity.id, entry->identity.version,
        entry->abi[0] ? ", ABI " : "", entry->abi);
    return true;
  }
  const struct package_provided_abi *provided = provided_abi();
  if (!client.provided_readable) {
    snprintf(client.message, sizeof(client.message),
        "%s cannot be checked: this system's ABI is unreadable (%s)", text, PACKAGE_OS_RELEASE);
  } else if (hidden_builds(wanted.identity.id, version, wanted.repository)) {
    snprintf(client.message, sizeof(client.message),
        "%s is offered only for other systems; none for ABI %s", text, provided->text);
  } else {
    snprintf(client.message, sizeof(client.message),
        "%s is not offered by any repository; this system provides ABI %s", text, provided->text);
  }
  return fail(client.message);
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
  if (!record_fetch(offered) ||
      !fetch(client.repositories.entries[offered->repository].url, offered->entry.archive,
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
    if (index != SIZE_MAX && (!dependency->version[0] ||
        !strcmp(selection->entries[index].version, dependency->version))) {
      continue;
    }
    const struct pkg_identity *owner = &manifest->identity;
    if (index == SIZE_MAX) {
      snprintf(client.message, sizeof(client.message),
          "%s %s depends on %s%s%s, which the proposed set lacks; remove %s first to drop it",
          owner->id, owner->version, dependency->id, dependency->version[0] ? "@" : "",
          dependency->version, owner->id);
    } else {
      snprintf(client.message, sizeof(client.message),
          "%s %s depends on %s@%s, but the proposed set holds %s",
          owner->id, owner->version, dependency->id, dependency->version,
          selection->entries[index].version);
    }
    return fail(client.message);
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
  const struct offered_package **chosen = calloc(argc, sizeof(*chosen));
  if (!requested || !chosen) {
    free(requested);
    free(chosen);
    return fail("cannot allocate package requests");
  }
  bool ok = true;
  for (int i = 0; ok && i < argc; ++i) {
    struct package_request want;
    ok = request(argv[i], &want);
    const char *version = want.explicit_version ? want.identity.version : NULL;
    const struct offered_package *entry = NULL;
    ok = ok && resolve(want.identity.id, version, want.repository, false, &entry);
    if (ok && !entry) {
      size_t hidden = hidden_builds(want.identity.id, version, want.repository);
      if (hidden && !client.provided_readable) {
        snprintf(client.message, sizeof(client.message),
            "%s has %zu build%s, but this system's ABI is unreadable (%s)",
            argv[i], hidden, hidden == 1 ? "" : "s", PACKAGE_OS_RELEASE);
        ok = fail(client.message);
      } else if (hidden) {
        snprintf(client.message, sizeof(client.message),
            "%s is not available for this system (%zu build%s for other systems)", argv[i], hidden,
            hidden == 1 ? "" : "s");
        ok = fail(client.message);
      } else {
        snprintf(client.message, sizeof(client.message), "%s is not available", argv[i]);
        ok = fail(client.message);
      }
    }
    if (ok) {
      requested[i] = entry->entry.identity;
      chosen[i] = entry;
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
    const struct offered_package *explicit_request = NULL;
    for (int j = 0; j < argc; ++j) {
      if (!strcmp(selection->entries[i].id, requested[j].id)) {
        explicit_request = chosen[j];
      }
    }
    struct pkg_manifest manifest = {0};
    handle_t existing;
    bool present = pkg_tree_open(client.packages, &selection->entries[i], PKG_READ_RIGHTS,
        &existing, &client.reason);
    if (present) handle_close(existing);
    client.reason = NULL;
    if (explicit_request) {
      ok = install_version(explicit_request, &manifest);
    } else if (present) {
      /* A stored tree is immutable. One carried over is trusted by its
       * manifest; a dependency this set adds is hashed in place, not fetched. */
      ok = i < initial_count ? trust_version(&selection->entries[i], &manifest) :
          verify_version(&selection->entries[i], &manifest);
    } else {
      const struct pkg_identity *wanted = &selection->entries[i];
      const struct offered_package *entry = NULL;
      ok = resolve(wanted->id, wanted->version[0] ? wanted->version : NULL, ANY_REPOSITORY,
          true, &entry);
      if (ok && !entry) {
        snprintf(client.message, sizeof(client.message),
            "dependency %s%s%s is not available for this system in the configured repositories",
            wanted->id, wanted->version[0] ? "@" : "", wanted->version);
        ok = fail(client.message);
      }
      if (ok) {
        selection->entries[i] = entry->entry.identity;
        ok = install_version(entry, &manifest);
      }
    }
    for (size_t d = 0; ok && d < manifest.dependency_count; ++d) {
      ok = select_version(selection, &manifest.dependencies[d], false);
    }
    pkg_manifest_free(&manifest);
  }
  free(requested);
  free(chosen);
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

/* The repository IDENTITY was fetched from in this operation, if any. */
static const char *origin(const struct pkg_identity *identity)
{
  for (size_t i = 0; i < client.fetched_count; ++i) {
    const struct pkg_identity *fetched = &client.fetched[i]->entry.identity;
    if (!strcmp(fetched->id, identity->id) && !strcmp(fetched->version, identity->version)) {
      return client.repositories.entries[client.fetched[i]->repository].name;
    }
  }
  return NULL;
}

static bool review(const struct pkg_selection *before, const struct pkg_selection *after)
{
  puts("Proposed package set (new launches only):");
  for (size_t i = 0; i < after->count; ++i) {
    size_t old = find_package(before, after->entries[i].id);
    if (old == SIZE_MAX || strcmp(before->entries[old].version, after->entries[i].version)) {
      const char *name = origin(&after->entries[i]);
      printf("  %s %s@%s%s%s%s\n", old == SIZE_MAX ? "add" : "select",
          after->entries[i].id, after->entries[i].version,
          name ? " (" : "", name ? name : "", name ? ")" : "");
    }
  }
  for (size_t i = 0; i < before->count; ++i) {
    if (find_package(after, before->entries[i].id) == SIZE_MAX) {
      printf("  remove %s@%s\n", before->entries[i].id, before->entries[i].version);
    }
  }
  if (client.yes) {
    puts("Switching to this package set (--yes).");
    return true;
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
  bool current_valid = pkg_selection_load(client.packages, &before, &client.reason);
  bool reindex = !strcmp(command, "reindex");
  bool ok = current_valid;
  struct pkg_projection projection = {0};
  struct pkg_sdk sdk = {0};
  if (reindex) {
    ok = ok && stale_check(&before, NULL, 0, "reindex", 0, NULL) && verify_selection(&before, NULL) &&
        pkg_commands_build(client.packages, &before, &projection, &client.reason) &&
        pkg_sdk_build(client.packages, &before, &sdk, &client.reason) &&
        pkg_reindex(client.packages, before.number, &projection, &sdk, &client.reason);
    if (ok) puts("pkg: active indexes bootstrapped; selection unchanged");
  } else if (ok) {
    ok = pkg_selection_load(client.packages, &after, &client.reason);
    if (ok && !strcmp(command, "install")) {
      ok = prepare_install(&after, argc, argv);
    } else if (ok) {
      ok = prepare_remove(&after, argc, argv);
    }
  }
  if (ok && !reindex) {
    ok = pkg_commands_build(client.packages, &after, &projection, &client.reason) &&
        pkg_sdk_build(client.packages, &after, &sdk, &client.reason);
  }
  if (ok && !reindex) {
    ok = review(&before, &after);
    if (!ok && !client.reason) client.reason = "change cancelled";
  }
  if (ok && !reindex && console_passthrough(client.terminal.input, &client.passthrough) != CALL_OK) {
    ok = fail("cannot guard the confirmed set switch");
  }
  if (ok && !reindex) {
    ok = pkg_publish(client.packages, &after, &projection, &sdk, &uncertain,
        &client.reason);
  }
  /* Root records are the set's packages with files. Say where a newly
   * installed one is readable. */
  for (size_t i = 0; ok && !reindex && i < projection.roots; ++i) {
    const struct package_command_record *root = &projection.records[projection.commands + i];
    if (find_package(&before, root->id) == SIZE_MAX) {
      printf("pkg: %s is read as pkg://%s in spaces with a pkg root\n", root->id, root->id);
    }
  }
  free(projection.records);
  free(sdk.bytes);
  pkg_selection_free(&after);
  pkg_selection_free(&before);
  handle_close(lock);
  if (!uncertain) ok = release_writer_lock(client.packages, ok);
  return ok;
}

/* add checks the repository the way every later read will, before it is saved. */
static bool repository_available(const struct pkg_repository *candidate)
{
  struct pkg_entry *entries = NULL;
  size_t count = 0;
  bool ok = read_repository(candidate, &entries, &count);
  free(entries);
  if (!ok) {
    fprintf(stderr, "pkg: not a usable package repository: %s: %s\n", candidate->url,
        client.reason ? client.reason : "index validation failed");
    return fail("repository configuration unchanged");
  }
  return true;
}

static bool repository_command(const char *command, const char *name, const char *url,
    bool unsigned_index)
{
  if (!strcmp(command, "list")) {
    if (pkg_repositories_load(client.config, &client.repositories, &client.reason) != CALL_OK) {
      return false;
    }
    for (size_t i = 0; i < client.repositories.count; ++i) {
      printf("%s %s%s\n", client.repositories.entries[i].name, client.repositories.entries[i].url,
          client.repositories.entries[i].unsigned_index ? " (unsigned)" : "");
    }
    if (!client.repositories.count) puts("No repositories; use pkg repository add NAME URL.");
    return true;
  }
  if (!package_id_valid(name)) {
    return fail("repository names follow the package ID rules");
  }
  /* Only add takes a URL; remove names the repository alone. */
  char normalized[PKG_BASE_URL_MAX + 1];
  if (url) {
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
  /* Under the lock, add records the index it accepted, like any writer. */
  client.packages = client.management;
  bool uncertain = false;
  bool ok = pkg_repositories_load(client.config_write, &client.repositories,
      &client.reason) == CALL_OK;
  struct pkg_repositories *repositories = &client.repositories;
  size_t index = 0;
  while (ok && index < repositories->count && strcmp(repositories->entries[index].name, name)) {
    ++index;
  }
  size_t same_url = 0;
  while (ok && url && same_url < repositories->count &&
      strcmp(repositories->entries[same_url].url, url)) {
    ++same_url;
  }
  bool changed = false;
  if (ok && !strcmp(command, "add")) {
    if (index < repositories->count) {
      snprintf(client.message, sizeof(client.message),
          "repository %s is already configured", name);
      ok = fail(client.message);
    } else if (same_url < repositories->count) {
      snprintf(client.message, sizeof(client.message),
          "%s is already configured as %s", url, repositories->entries[same_url].name);
      ok = fail(client.message);
    } else if (repositories->count == PKG_REPOSITORIES_MAX) {
      ok = fail("at most eight repositories may be configured");
    } else {
      struct pkg_repository candidate = {.unsigned_index = unsigned_index};
      strcpy(candidate.name, name);
      strcpy(candidate.url, url);
      if (repository_available(&candidate)) {
        repositories->entries[repositories->count++] = candidate;
        changed = true;
      } else {
        ok = false;
      }
    }
  } else if (ok) {
    if (index == repositories->count) {
      snprintf(client.message, sizeof(client.message), "no repository named %s", name);
      ok = fail(client.message);
    } else {
      memmove(repositories->entries + index, repositories->entries + index + 1,
          (repositories->count - index - 1) * sizeof(repositories->entries[0]));
      --repositories->count;
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

/* Installed packages built for an ABI the release does not provide. They are
 * listed only: programs upgrade as ordinary packages (decision 73). */
static void list_uncovered(const struct pkg_entry *release)
{
  struct pkg_selection current = {0};
  if (!pkg_selection_load(client.packages, &current, &client.reason)) {
    puts("    (the active package set cannot be read)");
    client.reason = NULL;
    return;
  }
  size_t listed = 0;
  for (size_t i = 0; i < current.count; ++i) {
    handle_t tree;
    struct pkg_manifest manifest = {0};
    if (!pkg_tree_open(client.packages, &current.entries[i], PKG_READ_RIGHTS, &tree,
          &client.reason)) {
      continue;
    }
    bool read = pkg_tree_manifest(tree, &manifest, &client.reason);
    handle_close(tree);
    bool covered = !read || !pkg_kind_bound(manifest.kind);
    for (size_t j = 0; !covered && j < release->provides_count; ++j) {
      covered = !strcmp(release->provides[j], manifest.abi);
    }
    if (!covered) {
      printf("    %s@%s (abi %s)\n", current.entries[i].id, current.entries[i].version,
          manifest.abi);
      ++listed;
    }
    pkg_manifest_free(&manifest);
  }
  if (!listed) {
    puts("    none");
  }
  pkg_selection_free(&current);
  client.reason = NULL;
}

static bool upgrade_review(const struct pkg_entry *release)
{
  const struct package_provided_abi *provided = provided_abi();
  char provides[PKG_PROVIDES_MAX * (PACKAGE_ABI_MAX + 2)] = "";
  for (size_t i = 0; i < release->provides_count; ++i) {
    append(provides, sizeof(provides), ", ", release->provides[i]);
  }
  puts("Release upgrade (takes effect at the next boot):");
  if (provided->release[0]) {
    printf("  running: %s\n", provided->release);
  } else {
    printf("  running: a development build (%s)\n", provided->text);
  }
  printf("  new: %s@%s, %llu bytes, provides %s\n", release->identity.id,
      release->identity.version, (unsigned long long)release->size, provides);
  puts("  installed packages it does not provide an ABI for:");
  list_uncovered(release);
  puts("  the current boot selection will be replaced; static rescue remains available.");
  if (client.yes) {
    puts("Staging this release (--yes).");
    return true;
  }
  if (client.terminal.input == HANDLE_INVALID || client.terminal.output == HANDLE_INVALID) {
    return fail("a release upgrade requires an interactive console review");
  }
  char line[16];
  struct term_line_result result = term_read_line(&client.terminal,
      "Stage this release? [y/N] ", line, sizeof(line));
  if (result.status != TERM_LINE_OK || result.limit_reached ||
      (strcmp(line, "y") && strcmp(line, "Y"))) {
    return fail("upgrade cancelled; nothing was written");
  }
  return true;
}

/* The release's own os-release must name it, and supplies the revision the
 * publisher records. */
static bool release_revision(handle_t tree, const char *version, char revision[65])
{
  handle_t file;
  char *bytes = NULL;
  size_t size = 0;
  bool ok = directory_lookup(tree, "os-release", DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
      &file) == CALL_OK && read_snapshot(file, &bytes, &size);
  if (file != HANDLE_INVALID) {
    handle_close(file);
  }
  struct literal *document = NULL;
  struct literal_diagnostic diagnostic;
  ok = ok && literal_parse(bytes, size, &document, &diagnostic);
  free(bytes);
  const struct literal_value *release = ok ? literal_field(document->root, "release") : NULL;
  const struct literal_value *named = ok ? literal_field(document->root, "revision") : NULL;
  ok = release && release->kind == LITERAL_STRING && !strcmp(release->as.string.bytes, version) &&
      named && named->kind == LITERAL_STRING && named->as.string.size &&
      named->as.string.size < 65 && package_abi_valid(named->as.string.bytes);
  if (ok) {
    memcpy(revision, named->as.string.bytes, named->as.string.size + 1);
  }
  if (document) {
    literal_free(document);
  }
  return ok || fail("the release's os-release does not name this release and a revision");
}

/* FILE_TRANSFER to the ESP takes RAM sources only, so the publisher reads each
 * verified payload from a copy in the RAM workspace. */
static bool ram_payload(handle_t tree, const char *name, handle_t workspace, handle_t *copy,
    uint64_t *size)
{
  handle_t source = HANDLE_INVALID;
  bool ok = directory_lookup(tree, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &source) == CALL_OK &&
      file_size(source, size) == CALL_OK &&
      directory_create(workspace, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ | FILE_RIGHT_WRITE,
          copy) == CALL_OK;
  static unsigned char buffer[64 * 1024];
  for (uint64_t offset = 0; ok && offset < *size;) {
    size_t wanted = *size - offset > sizeof(buffer) ? sizeof(buffer) : (size_t)(*size - offset);
    size_t count = 0, written = 0;
    ok = file_read(source, offset, buffer, wanted, &count) == CALL_OK && count;
    while (ok && written < count) {
      size_t step = 0;
      ok = file_write(*copy, offset + written, buffer + written, count - written, &step) == CALL_OK &&
          step;
      written += step;
    }
    offset += count;
  }
  if (source != HANDLE_INVALID) {
    handle_close(source);
  }
  return ok || fail("cannot copy a staged release payload into the RAM workspace");
}

/* Fetch, verify and stage RELEASE in packages/.upgrade/, then publish it as
 * the next boot attempt. Staging is removed afterwards, and at the start of
 * the next upgrade if this one was interrupted (decision 74). */
static bool stage_and_publish(const struct pkg_entry *release, const struct pkg_repository *source,
    const struct pkg_boot_evidence *evidence, handle_t issuer)
{
  struct pkg_progress progress;
  pkg_progress_init(&progress, client.clock, client.terminal.output != HANDLE_INVALID,
      release->identity.id, release->identity.version);
  struct pkg_archive_progress observer = { pkg_progress_archive, &progress };
  handle_t staging = HANDLE_INVALID, destination = HANDLE_INVALID, tree = HANDLE_INVALID;
  handle_t snapshot = HANDLE_INVALID, kernel = HANDLE_INVALID, archive = HANDLE_INVALID;
  handle_t tmp = HANDLE_INVALID, workspace = HANDLE_INVALID;
  struct pkg_manifest manifest = {0};
  uint64_t kernel_size = 0, archive_size = 0, now = 0;
  char revision[65], workspace_name[48];
  bool ok = pkg_upgrade_clear(client.packages, UPGRADE_STAGING) ||
      fail("cannot remove an earlier upgrade's staging");
  ok = ok && pkg_directory(client.packages, UPGRADE_STAGING, true, PKG_WRITE_RIGHTS,
      &staging, &client.reason);
  ok = ok && (directory_create(staging, "release", DIRECTORY_KIND_DIRECTORY, PKG_WRITE_RIGHTS,
      &destination) == CALL_OK || fail("cannot create the release staging"));
  if (ok) {
    pkg_progress_fetch_begin(&progress, release->size);
    ok = fetch(source->url, release->archive, false, release->size, &progress, &snapshot);
  }
  if (ok) {
    pkg_progress_fetch_end(&progress, release->size);
    ok = pkg_archive(snapshot, release, destination, &manifest, &observer, &client.reason);
    pkg_progress_abort(&progress);
  }
  ok = ok && (pkg_kind_release(manifest.kind) || fail("the staged archive is not a release"));
  ok = ok && (directory_lookup(destination, "tree", DIRECTORY_KIND_DIRECTORY, PKG_READ_RIGHTS,
      &tree) == CALL_OK || fail("the staged release has no tree"));
  ok = ok && release_revision(tree, release->identity.version, revision);
  /* The publisher verifies through a RAM workspace; tmp:// is RAM-backed. */
  ok = ok && (path_root("tmp", &tmp) == CALL_OK || fail("tmp:// is unavailable"));
  if (ok && clock_now(client.clock, &now) == CALL_OK) {
    snprintf(workspace_name, sizeof(workspace_name), "pkg-upgrade-%llu", (unsigned long long)now);
    ok = directory_create(tmp, workspace_name, DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES |
        DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE |
        DIRECTORY_RIGHT_FILESYSTEM_INFO, &workspace) == CALL_OK ||
        fail("cannot create a RAM workspace in tmp://");
  } else if (ok) {
    ok = fail("cannot name a RAM workspace");
  }
  ok = ok && ram_payload(tree, "caelum.elf", workspace, &kernel, &kernel_size) &&
      ram_payload(tree, "initrd.cpio", workspace, &archive, &archive_size);
  if (ok) {
    const struct pkg_upgrade_publish request = {.issuer = issuer, .clock = client.clock,
      .random = bootstrap_capability("random"), .workspace = workspace, .kernel = kernel,
      .archive = archive, .kernel_size = kernel_size, .archive_size = archive_size,
      .revision = revision, .evidence = evidence};
    ok = pkg_upgrade_publish(&request, &client.reason);
  }
  pkg_manifest_free(&manifest);
  handle_t opened[] = {kernel, archive, tree, destination, staging, snapshot, workspace};
  for (size_t i = 0; i < sizeof(opened) / sizeof(opened[0]); ++i) {
    if (opened[i] != HANDLE_INVALID) {
      handle_close(opened[i]);
    }
  }
  if (workspace != HANDLE_INVALID && !pkg_upgrade_clear(tmp, workspace_name)) {
    fprintf(stderr, "pkg: cannot remove tmp://%s\n", workspace_name);
  }
  if (tmp != HANDLE_INVALID) {
    handle_close(tmp);
  }
  if (!pkg_upgrade_clear(client.packages, UPGRADE_STAGING)) {
    fputs("pkg: cannot remove the release staging; the next upgrade removes it\n", stderr);
  }
  return ok;
}

/* pkg upgrade [VERSION] (docs/wip/pkg-upgrade.md, step 3): the newest release
 * in the signed releases repository, or the one named, staged as the next boot
 * attempt. */
static bool upgrade(const char *version)
{
  handle_t issuer = bootstrap_capability(PACKAGE_UPGRADE_SOURCE);
  handle_t evidence_file = bootstrap_capability(PACKAGE_EVIDENCE_SOURCE);
  if (issuer == HANDLE_INVALID || evidence_file == HANDLE_INVALID) {
    return fail("upgrade needs an installed system booted from its own boot entry, in a space "
        "with package_upgrade = true; live media and rescue boots cannot upgrade");
  }
  struct pkg_boot_evidence evidence;
  if (!pkg_boot_evidence_read(evidence_file, &evidence, &client.reason)) {
    return false;
  }
  const struct package_provided_abi *provided = provided_abi();
  if (!client.provided_readable) {
    return fail("cannot read boot://os-release");
  }
  if (!package_volume(client.management, true)) {
    return false;
  }
  client.packages = client.management;
  handle_t lock;
  if (directory_create(client.packages, PKG_LOCK, DIRECTORY_KIND_DIRECTORY,
        DIRECTORY_RIGHT_ENUMERATE, &lock) != CALL_OK) {
    return fail("writer lock unavailable; use pkg unlock only after the prior client has stopped");
  }
  bool ok = pkg_repositories_load(client.config, &client.repositories, &client.reason) == CALL_OK;
  const struct pkg_repository *source = NULL;
  for (size_t r = 0; ok && r < client.repositories.count; ++r) {
    if (!strcmp(client.repositories.entries[r].name, RELEASES_REPOSITORY)) {
      source = &client.repositories.entries[r];
    }
  }
  if (ok && !source) {
    ok = fail("no releases repository is configured");
  } else if (ok && source->unsigned_index) {
    ok = fail("the releases repository is always signed; its entry must not say signed = false");
  }
  struct pkg_entry *entries = NULL;
  size_t count = 0;
  const struct pkg_entry *chosen = NULL;
  if (ok) {
    printf("pkg: reading repository %s\n", source->name);
    ok = read_repository(source, &entries, &count);
  }
  for (size_t i = 0; ok && i < count; ++i) {
    if (pkg_kind_release(entries[i].kind) && !strcmp(entries[i].identity.id, RELEASE_ID) &&
        (!version || !strcmp(entries[i].identity.version, version))) {
      chosen = &entries[i];
    }
  }
  if (ok && !chosen) {
    if (version) {
      snprintf(client.message, sizeof(client.message),
          "the releases repository does not offer %s@%s", RELEASE_ID, version);
    } else {
      snprintf(client.message, sizeof(client.message),
          "the releases repository offers no %s release", RELEASE_ID);
    }
    ok = fail(client.message);
  } else if (ok && !strcmp(chosen->identity.version, provided->release)) {
    snprintf(client.message, sizeof(client.message), "%s is already running",
        chosen->identity.version);
    ok = fail(client.message);
  }
  ok = ok && ensure_crypto() && upgrade_review(chosen) &&
      stage_and_publish(chosen, source, &evidence, issuer);
  if (ok) {
    printf("pkg: upgrade staged: reboot to run %s\n", chosen->identity.version);
  }
  free(entries);
  handle_close(lock);
  return release_writer_lock(client.packages, ok);
}

static int usage(void)
{
  fputs("usage: pkg [--timeout SECONDS] [--yes] status | list [--available] | check [REPO/]ID[@VERSION] | install [REPO/]ID[@VERSION]... | remove ID... | verify | reindex | unlock | upgrade [VERSION] | repository list | repository add [--unsigned] NAME URL | repository remove NAME\nRepository names follow the package ID rules; URLs accept an optional final / (2048 bytes including the stored final /).\n", stderr);
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  client.clock = bootstrap_capability("clock");
  client.management = bootstrap_capability(PACKAGE_MANAGEMENT_SOURCE);
  client.packages = bootstrap_capability(PACKAGE_INSPECT_SOURCE);
  client.config = bootstrap_capability(PACKAGE_CONFIG_SOURCE);
  client.config_write = bootstrap_capability(PACKAGE_CONFIG_WRITE_SOURCE);
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
  if (arg < argc && !strcmp(argv[arg], "--yes")) {
    client.yes = true;
    ++arg;
  }
  if (arg >= argc) return usage();
  const char *command = argv[arg++];
  int count = argc - arg;
  char **arguments = argv + arg;
  bool repository = !strcmp(command, "repository");
  bool unsigned_index = repository && count == 4 && !strcmp(arguments[0], "add") &&
      !strcmp(arguments[1], "--unsigned");
  if (unsigned_index) {
    /* add --unsigned NAME URL: drop the flag from the positional arguments. */
    arguments[1] = arguments[0];
    ++arguments;
    --count;
  }
  if (repository && (count < 1 ||
      (!strcmp(arguments[0], "list") ? count != 1 :
      !strcmp(arguments[0], "add") ? count != 3 :
      !strcmp(arguments[0], "remove") ? count != 2 : true))) {
    return usage();
  }
  bool mutation = !strcmp(command, "install") || !strcmp(command, "remove") ||
      !strcmp(command, "reindex") || !strcmp(command, "unlock");
  if (!repository && ((!strcmp(command, "install") || !strcmp(command, "remove")) ? count < 1 :
      !strcmp(command, "check") ? count != 1 :
      !strcmp(command, "upgrade") ? count > 1 :
      !strcmp(command, "list") ? count > 1 || (count && strcmp(arguments[0], "--available")) : count != 0)) {
    return usage();
  }
  bool needs_hash = (mutation && strcmp(command, "unlock")) || !strcmp(command, "verify");
  if (needs_hash && !ensure_crypto()) {
    fputs("pkg: signature crypto initialization unavailable\n", stderr);
    return EXIT_FAILURE;
  }
  bool ok = false;
  if (!strcmp(command, "upgrade")) {
    ok = upgrade(count ? arguments[0] : NULL);
  } else if (repository) {
    ok = repository_command(arguments[0], count > 1 ? arguments[1] : NULL,
        count > 2 ? arguments[2] : NULL, unsigned_index);
  } else if (mutation) {
    ok = mutate(command, count, arguments);
  } else if (!strcmp(command, "check")) {
    ok = check(arguments[0]);
  } else if (!strcmp(command, "list") && count) {
    ok = load_repositories();
    for (size_t i = 0; ok && i < client.offered_count; ++i) {
      const struct pkg_entry *entry = &client.offered[i].entry;
      printf("%s/%s@%s %s%s%s %llu bytes\n",
          client.repositories.entries[client.offered[i].repository].name,
          entry->identity.id, entry->identity.version,
          entry->kind, entry->abi[0] ? " abi " : "", entry->abi,
          (unsigned long long)entry->size);
    }
    if (ok && client.hidden_count) {
      printf(client.provided_readable ? "%zu builds for other systems hidden\n" :
          "%zu builds hidden: this system's ABI is unreadable (" PACKAGE_OS_RELEASE ")\n",
          client.hidden_count);
    }
  } else if (!strcmp(command, "status") || !strcmp(command, "list") || !strcmp(command, "verify")) {
    struct pkg_selection selection = {0};
    ok = package_volume(client.packages, false) &&
        pkg_selection_load(client.packages, &selection, &client.reason);
    if (ok) {
      print_selection(&selection);
      if (!strcmp(command, "verify")) {
        ok = stale_check(&selection, NULL, 0, "verify", 0, NULL) && verify_selection(&selection, NULL);
        struct pkg_projection projection = {0};
        struct pkg_sdk sdk = {0};
        ok = ok && pkg_commands_build(client.packages, &selection, &projection,
            &client.reason) &&
            pkg_sdk_build(client.packages, &selection, &sdk, &client.reason) &&
            pkg_indexes_verify(client.packages, selection.number, &projection, &sdk,
                &client.reason);
        free(projection.records);
        free(sdk.bytes);
        if (ok) puts("pkg: selected package files and indexes verified");
      }
    }
    pkg_selection_free(&selection);
  } else {
    release_publish_guard();
    if (client.crypto_ready) pkg_crypto_close();
    free(client.offered);
    free(client.hidden);
    free(client.fetched);
    return usage();
  }
  release_publish_guard();
  if (client.crypto_ready) pkg_crypto_close();
  free(client.offered);
  free(client.hidden);
  free(client.fetched);
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
