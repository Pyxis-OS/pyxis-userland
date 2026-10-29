#include "legacy_menu.h"
#include "installer.h"
#include "esp_directory.h"
#include "esp_payload.h"
#include "esp_transfer.h"
#include "menu_ownership.h"
#include "publication_snapshot.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>

#define LEGACY_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define LEGACY_MANIFEST_MAX 2048u

enum legacy_role { LEGACY_NORMAL, LEGACY_PREVIOUS, LEGACY_RESCUE, LEGACY_ROLES };
static const char *const legacy_titles[LEGACY_ROLES] = {
  "/Pyxis OS 0.0.9 (Caelum)\n", "/Pyxis OS 0.0.8 (previous)\n", "/Pyxis OS 0.0.9 (rescue)\n"
};
static const char *const payload_names[INSTALL_ESP_PAYLOAD_COUNT + 1] = {
  "kernel.elf", "boot.arc", "revision.txt", "entry.txt", "ownership.record"
};

struct legacy_attempt {
  char revision[65], attempt[33];
  char paths[INSTALL_ESP_PAYLOAD_COUNT][INSTALL_ESP_PAYLOAD_PATH_BYTES];
  struct config_file_digest digests[INSTALL_ESP_PAYLOAD_COUNT];
  struct config_blob manifest;
};

static bool same_name(const char *left, const char *right)
{
  for (;; ++left, ++right) {
    unsigned a = (unsigned char)*left, b = (unsigned char)*right;
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
    if (!a) return true;
  }
}

static bool mentions_pyxis(const unsigned char *bytes, size_t size)
{
  for (size_t i = 0; i + 5 <= size; ++i) {
    char word[6];
    memcpy(word, bytes + i, 5);
    word[5] = 0;
    if (same_name(word, "pyxis")) return true;
  }
  return false;
}

/* Only headings delimit entries. Extra directives in a recognized entry
 * refuse, even when separated from its authored lines by blank lines. */
static bool menu_spans(const struct config_blob *menu, struct literal_span spans[LEGACY_ROLES],
    bool *candidate)
{
  int previous = -1;
  for (size_t start = 0; start < menu->size;) {
    size_t end = start;
    while (end < menu->size && menu->bytes[end] != '\n') ++end;
    if (end < menu->size) ++end;
    if (menu->bytes[start] == '/') {
      if (previous >= 0) spans[previous].end = start;
      previous = -1;
      for (unsigned role = 0; role < LEGACY_ROLES; ++role) {
        size_t size = strlen(legacy_titles[role]);
        if (size == end - start && !memcmp(menu->bytes + start, legacy_titles[role], size)) {
          *candidate = true;
          if (spans[role].end) return false;
          spans[role] = (struct literal_span){start, menu->size};
          previous = (int)role;
          break;
        }
      }
      if (previous < 0 && mentions_pyxis(menu->bytes + start, end - start)) {
        *candidate = true;
        return false;
      }
    }
    start = end;
  }
  return !*candidate || (spans[LEGACY_NORMAL].end && spans[LEGACY_RESCUE].end &&
      spans[LEGACY_NORMAL].start < spans[LEGACY_RESCUE].start &&
      (!spans[LEGACY_PREVIOUS].end ||
        (spans[LEGACY_NORMAL].start < spans[LEGACY_PREVIOUS].start &&
          spans[LEGACY_PREVIOUS].start < spans[LEGACY_RESCUE].start)));
}

static bool attempt_from_menu(const struct config_blob *menu, struct literal_span span,
    enum legacy_role role, const uint8_t esp_guid[16], struct legacy_attempt *attempt)
{
  char esp[37], prefix[256];
  install_guid_text(esp_guid, esp);
  int size = snprintf(prefix, sizeof(prefix), "%s  protocol: limine\n  path: guid(%s):/EFI/pyxis/",
      legacy_titles[role], esp);
  if (size <= 0 || (size_t)size >= sizeof(prefix) || span.end - span.start <= (size_t)size ||
      memcmp(menu->bytes + span.start, prefix, (size_t)size)) return false;
  const char *cursor = (char *)menu->bytes + span.start + size;
  const char *end = (char *)menu->bytes + span.end;
  const char *slash = memchr(cursor, '/', (size_t)(end - cursor));
  if (!slash || slash == cursor || slash - cursor >= (ptrdiff_t)sizeof(attempt->revision)) return false;
  memcpy(attempt->revision, cursor, (size_t)(slash - cursor));
  cursor = slash + 1;
  if (!config_revision_valid(attempt->revision) || end - cursor < 44) return false;
  memcpy(attempt->attempt, cursor, 32);
  if (!config_transaction_valid(attempt->attempt) || memcmp(cursor + 32, "/kernel.elf\n", 12)) return false;
  for (unsigned i = 0; i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
    if (!install_esp_payload_path(attempt->revision, attempt->attempt, i, attempt->paths[i])) return false;
  }
  return true;
}

static bool legacy_entry_matches(const struct config_blob *menu, struct literal_span *span,
    enum legacy_role role, const struct legacy_attempt *attempt,
    const struct config_target_facts *facts, uint64_t pool_partition)
{
  char disk[37], esp[37], text[1400];
  install_guid_text(facts->disk_guid, disk);
  install_guid_text(facts->partition_guid, esp);
  int size = snprintf(text, sizeof(text), "%s  protocol: limine\n  path: guid(%s):/%s\n"
      "  module_path: guid(%s):/%s\n  paging_mode: 4level\n  kaslr: no\n"
      "  cmdline: " INSTALL_BOOT_INIT_OPTION " mount.disk=%s mount.pool=%llu%s\n",
      legacy_titles[role], esp, attempt->paths[INSTALL_ESP_KERNEL], esp,
      attempt->paths[INSTALL_ESP_ARCHIVE], disk, (unsigned long long)pool_partition,
      role == LEGACY_RESCUE ? " " INSTALL_RESCUE_OPTION : " nvme.write=1");
  if (size <= 0 || (size_t)size >= sizeof(text) || span->end - span->start < (size_t)size ||
      memcmp(menu->bytes + span->start, text, (size_t)size)) return false;
  size_t entry_end = span->start + size;
  for (size_t cursor = entry_end; cursor < span->end;) {
    if (menu->bytes[cursor] == '\n') {
      ++cursor;
    } else if (menu->bytes[cursor] == '#') {
      while (cursor < span->end && menu->bytes[cursor] != '\n') ++cursor;
    } else {
      return false;
    }
  }
  span->end = entry_end;
  return true;
}

static bool attempt_open(handle_t root, const struct legacy_attempt *attempt, handle_t *parent)
{
  const char *names[] = {"EFI", "pyxis", attempt->revision, attempt->attempt};
  handle_t held = root;
  bool owned = false, ok = true;
  *parent = HANDLE_INVALID;
  for (unsigned i = 0; ok && i < sizeof(names) / sizeof(*names); ++i) {
    handle_t next = HANDLE_INVALID;
    ok = install_esp_child_open(held, names[i], DIRECTORY_KIND_DIRECTORY,
        LEGACY_DIRECTORY_READ, false, &next) == CALL_OK;
    if (owned) ok = handle_close(held) == CALL_OK && ok;
    held = next;
    owned = held != HANDLE_INVALID;
  }
  if (ok) *parent = held;
  else if (owned) handle_close(held);
  return ok;
}

static bool attempt_contents(handle_t parent)
{
  struct directory_cursor cursor = {0};
  unsigned seen = 0;
  for (;;) {
    char name[256];
    struct directory_enumerate_reply entry;
    if (directory_enumerate(parent, &cursor, name, sizeof(name), &entry) != CALL_OK) return false;
    if (entry.outcome == DIRECTORY_END) return seen == ((1u << 5) - 1);
    if (entry.outcome != DIRECTORY_ENTRY || entry.kind != DIRECTORY_KIND_FILE) return false;
    unsigned slot = 0;
    while (slot < 5 && !same_name(name, payload_names[slot])) ++slot;
    if (slot == 5 || (seen & (1u << slot))) return false;
    seen |= 1u << slot;
    cursor = entry.cursor;
  }
}

static bool manifest_digest(const char **cursor, const char *end, const char *path,
    struct config_file_digest *digest)
{
  const char *line = *cursor;
  size_t size = strlen(path);
  if ((size_t)(end - line) <= size || memcmp(line, path, size) || line[size] != ' ') return false;
  line += size + 1;
  if (line == end || *line < '1' || *line > '9') return false;
  uint64_t bytes = 0;
  do {
    unsigned digit = *line++ - '0';
    if (bytes > (INSTALL_ESP_PAYLOAD_MAX_BYTES - digit) / 10) return false;
    bytes = bytes * 10 + digit;
  } while (line < end && *line >= '0' && *line <= '9');
  if (line == end || *line++ != ' ' || end - line < 65) return false;
  memcpy(digest->sha256, line, 64);
  digest->sha256[64] = 0;
  line += 64;
  if (*line++ != '\n') return false;
  digest->present = true;
  digest->size = bytes;
  if (!config_payload_digest_valid(digest)) return false;
  *cursor = line;
  return true;
}

static bool attempt_verify(handle_t root, handle_t ram, struct legacy_attempt *attempt,
    const struct config_target_facts *facts, uint64_t pool_partition)
{
  handle_t parent = HANDLE_INVALID;
  if (!attempt_open(root, attempt, &parent)) return false;
  struct config_blob revision = {0}, entry = {0}, expected_entry = {0};
  bool ok = attempt_contents(parent) && install_snapshot_file(parent, "ownership.record",
      LEGACY_MANIFEST_MAX, &attempt->manifest) && attempt->manifest.digest.present &&
      !memchr(attempt->manifest.bytes, 0, attempt->manifest.size);
  char header[192];
  int size = snprintf(header, sizeof(header), "PYXIS-BOOT-OWNERSHIP 1\nrevision %s\nattempt %s\n",
      attempt->revision, attempt->attempt);
  ok = ok && size > 0 && (size_t)size < sizeof(header) && attempt->manifest.size > (size_t)size &&
      !memcmp(attempt->manifest.bytes, header, (size_t)size);
  if (ok) {
    const char *cursor = (char *)attempt->manifest.bytes + size;
    const char *end = (char *)attempt->manifest.bytes + attempt->manifest.size;
    for (unsigned i = 0; ok && i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
      ok = manifest_digest(&cursor, end, attempt->paths[i], &attempt->digests[i]);
    }
    ok = ok && cursor == end;
  }
  for (unsigned i = 0; ok && i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
    handle_t file = HANDLE_INVALID;
    ok = install_esp_child_open(parent, payload_names[i], DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, false, &file) == CALL_OK &&
        install_esp_transfer_file(file, ram, true, attempt->digests[i].size,
            attempt->digests[i].sha256, INSTALL_ESP_PAYLOAD_MAX_BYTES) == CALL_OK;
    if (file != HANDLE_INVALID) ok = handle_close(file) == CALL_OK && ok;
  }
  struct install_boot_entry_paths paths = {attempt->paths[INSTALL_ESP_KERNEL], attempt->paths[INSTALL_ESP_ARCHIVE]};
  char expected_revision[66];
  size = snprintf(expected_revision, sizeof(expected_revision), "%s\n", attempt->revision);
  ok = ok && size > 0 && (size_t)size < sizeof(expected_revision) &&
      install_snapshot_file(parent, "revision.txt", sizeof(expected_revision), &revision) &&
      revision.digest.present && revision.size == (size_t)size &&
      !memcmp(revision.bytes, expected_revision, revision.size) &&
      install_boot_entry_render(&paths, facts->disk_guid, pool_partition,
          facts->partition_guid, &expected_entry) &&
      install_snapshot_file(parent, "entry.txt", CONFIG_FILE_MAX_BYTES, &entry) &&
      config_blob_equal(&entry, &expected_entry);
  config_blob_destroy(&revision);
  config_blob_destroy(&entry);
  config_blob_destroy(&expected_entry);
  return handle_close(parent) == CALL_OK && ok;
}

static bool menu_replace(const struct config_blob *current,
    const struct literal_span spans[LEGACY_ROLES], const struct config_blob *owned,
    struct config_blob *selected)
{
  struct config_owned_set *set = NULL;
  if (!config_owned_scan_limine("new boot entries", (char *)owned->bytes, owned->size, &set)) return false;
  const struct config_owned_span *normal = config_owned_find(set, "boot.normal");
  const struct config_owned_span *rescue = config_owned_find(set, "boot.rescue");
  bool ok = normal && rescue;
  size_t size = current->size;
  const struct config_owned_span *replacements[] = {normal, rescue};
  const struct literal_span old[] = {spans[LEGACY_NORMAL], spans[LEGACY_RESCUE]};
  for (unsigned i = 0; ok && i < 2; ++i) {
    size -= old[i].end - old[i].start;
    size_t added = replacements[i]->span.end - replacements[i]->span.start;
    ok = added <= CONFIG_FILE_MAX_BYTES && size <= CONFIG_FILE_MAX_BYTES - added;
    if (ok) size += added;
  }
  unsigned char *bytes = ok ? malloc(size) : NULL;
  ok = ok && bytes;
  size_t from = 0, used = 0;
  for (unsigned i = 0; ok && i < 2; ++i) {
    size_t unchanged = old[i].start - from;
    memcpy(bytes + used, current->bytes + from, unchanged);
    used += unchanged;
    size_t added = replacements[i]->span.end - replacements[i]->span.start;
    memcpy(bytes + used, owned->bytes + replacements[i]->span.start, added);
    used += added;
    from = old[i].end;
  }
  if (ok) {
    memcpy(bytes + used, current->bytes + from, current->size - from);
    ok = config_blob_set(selected, bytes, size);
  }
  free(bytes);
  config_owned_free(set);
  return ok;
}

bool install_legacy_menu_plan(handle_t root, handle_t workspace,
    const struct config_target_facts *facts, uint64_t pool_partition,
    const struct config_blob *current, const char *expected_revision,
    const struct install_boot_entry_paths *next, struct config_blob *selected,
    struct config_blob *evidence, bool *adopted, const char **reason)
{
  *adopted = false;
  *reason = "the legacy primary menu is edited, ambiguous or not bound to the selected installed pool";
  if (!facts || !config_target_valid(facts) || !pool_partition || !current ||
      !current->digest.present || !expected_revision || !config_revision_valid(expected_revision) ||
      memchr(current->bytes, 0, current->size)) return false;
  struct handle_info grant;
  struct file_info_reply info;
  if (handle_query(root, &grant) != CALL_OK || grant.kind != HANDLE_KIND_NATIVE ||
      grant.protocol != PROTOCOL_DIRECTORY || grant.transport ||
      directory_info(root, &info) != CALL_OK || !(info.valid & FILE_INFO_DOMAIN_VALID) ||
      info.domain != facts->domain) return false;
  struct literal_span spans[LEGACY_ROLES] = {0};
  bool candidate = false;
  if (!menu_spans(current, spans, &candidate)) return false;
  if (!candidate) {
    *reason = NULL;
    return true;
  }
  struct config_owned_set *markers = NULL;
  bool ok = config_owned_scan_limine("legacy primary menu", (char *)current->bytes, current->size, &markers);
  ok = ok && !config_owned_find(markers, "boot.normal") &&
      !config_owned_find(markers, "boot.rescue") && !config_owned_find(markers, "boot.previous");
  for (size_t i = 0; ok && markers && i < markers->count; ++i) {
    for (unsigned role = 0; ok && role < LEGACY_ROLES; ++role) {
      if (spans[role].end && markers->spans[i].span.start < spans[role].end &&
          spans[role].start < markers->spans[i].span.end) ok = false;
    }
  }
  config_owned_free(markers);
  struct legacy_attempt attempts[2] = {0};
  ok = ok && attempt_from_menu(current, spans[LEGACY_NORMAL], LEGACY_NORMAL,
      facts->partition_guid, &attempts[0]) &&
      (!strcmp(expected_revision, INSTALL_UNKNOWN_REVISION) ||
       !strcmp(attempts[0].revision, expected_revision)) &&
      legacy_entry_matches(current, &spans[LEGACY_NORMAL], LEGACY_NORMAL, &attempts[0], facts, pool_partition) &&
      legacy_entry_matches(current, &spans[LEGACY_RESCUE], LEGACY_RESCUE, &attempts[0], facts, pool_partition);
  unsigned count = spans[LEGACY_PREVIOUS].end ? 2 : 1;
  if (ok && count == 2) {
    ok = attempt_from_menu(current, spans[LEGACY_PREVIOUS], LEGACY_PREVIOUS,
        facts->partition_guid, &attempts[1]) && strcmp(attempts[0].revision, attempts[1].revision) &&
        legacy_entry_matches(current, &spans[LEGACY_PREVIOUS], LEGACY_PREVIOUS, &attempts[1], facts, pool_partition);
  }
  handle_t ram = HANDLE_INVALID;
  if (ok) {
    *reason = "the legacy primary menu's immutable attempt ownership or complete payload hashes cannot be verified";
    ok = install_esp_transfer_workspace(workspace, &ram) == CALL_OK;
  }
  for (unsigned i = 0; ok && i < count; ++i) {
    ok = attempt_verify(root, ram, &attempts[i], facts, pool_partition);
  }
  if (ram != HANDLE_INVALID) ok = handle_close(ram) == CALL_OK && ok;
  if (ok) {
    char bytes[LEGACY_MANIFEST_MAX * 2 + 128];
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
      int size = snprintf(bytes + used, sizeof(bytes) - used, "%zu\n", attempts[i].manifest.size);
      if (size <= 0 || (size_t)size >= sizeof(bytes) - used ||
          attempts[i].manifest.size > sizeof(bytes) - used - (size_t)size) {
        ok = false;
        break;
      }
      used += size;
      memcpy(bytes + used, attempts[i].manifest.bytes, attempts[i].manifest.size);
      used += attempts[i].manifest.size;
    }
    if (ok) ok = config_blob_set(evidence, bytes, used);
  }
  struct config_blob owned = {0};
  if (ok) {
    *reason = "cannot render the adopted primary menu";
    ok = install_boot_entry_render_owned(next, facts->disk_guid, pool_partition, &owned) &&
        menu_replace(current, spans, &owned, selected);
  }
  if (ok) {
    struct config_owned_set *combined = NULL;
    ok = config_owned_scan_limine("adopted primary menu", (char *)selected->bytes,
        selected->size, &combined);
    config_owned_free(combined);
  }
  config_blob_destroy(&owned);
  for (unsigned i = 0; i < 2; ++i) config_blob_destroy(&attempts[i].manifest);
  if (ok) {
    *adopted = true;
    *reason = NULL;
  }
  return ok;
}
