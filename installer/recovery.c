#include "recovery.h"
#include "boot_entry.h"
#include "esp_directory.h"
#include "esp_payload.h"
#include "esp_transfer.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOVERY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define RECOVERY_WRITE (RECOVERY_READ | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
#define RECOVERY_FILE_MAX INSTALL_ESP_PAYLOAD_MAX_BYTES
#define RECOVERY_MANIFEST_MAX 512u
#define RECOVERY_DIRECTORY "EFI/pyxis/recovery"
#define RECOVERY_MANIFEST RECOVERY_DIRECTORY "/ownership.record"
#define RECOVERY_FALLBACK "boot/limine.conf"

static const char *const recovery_names[] = {"kernel.elf", "boot.arc", "ownership.record"};

static bool root_bound(handle_t root, const struct config_target_facts *facts)
{
  struct handle_info grant;
  struct file_info_reply info;
  return handle_query(root, &grant) == CALL_OK && grant.kind == HANDLE_KIND_NATIVE &&
      grant.protocol == PROTOCOL_DIRECTORY && !grant.transport &&
      directory_info(root, &info) == CALL_OK && (info.valid & FILE_INFO_DOMAIN_VALID) &&
      info.domain == facts->domain;
}

static bool name_equal(const char *left, const char *right)
{
  for (;; ++left, ++right) {
    unsigned char a = *left, b = *right;
    if (a >= 'a' && a <= 'z') {
      a -= 'a' - 'A';
    }
    if (b >= 'a' && b <= 'z') {
      b -= 'a' - 'A';
    }
    if (a != b) {
      return false;
    }
    if (!a) {
      return true;
    }
  }
}

/* NOT_FOUND means actual absence, never an alias, a type mismatch or failed
 * enumeration. An existing alias cannot certify a canonical owned path. */
static enum call_status child(handle_t parent, const char *name, uint64_t kind,
    uint64_t rights, handle_t *result)
{
  enum call_status status = install_esp_child_open(parent, name, kind, rights, false, result);
  if (status != CALL_NOT_FOUND) {
    return status;
  }
  handle_t alias = HANDLE_INVALID;
  status = directory_lookup(parent, name, kind, rights, &alias);
  if (status == CALL_OK) {
    handle_close(alias);
    return CALL_DENIED;
  }
  return status;
}

static enum call_status parent_open(handle_t root, const char *path, uint64_t rights,
    bool create, handle_t *result)
{
  *result = HANDLE_INVALID;
  handle_t parent = root;
  bool owned = false;
  enum call_status status = CALL_BAD_REQUEST;
  while (*path) {
    const char *slash = strchr(path, '/');
    size_t size = slash ? (size_t)(slash - path) : strlen(path);
    char component[64];
    if (!size || size >= sizeof(component)) {
      break;
    }
    memcpy(component, path, size);
    component[size] = '\0';
    handle_t next = HANDLE_INVALID;
    status = child(parent, component, DIRECTORY_KIND_DIRECTORY, rights, &next);
    if (create && status == CALL_NOT_FOUND) {
      status = directory_create(parent, component, DIRECTORY_KIND_DIRECTORY, rights, &next);
      if (status == CALL_OK) {
        status = directory_sync(parent);
      }
    }
    if (owned && handle_close(parent) != CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    owned = false;
    if (status != CALL_OK) {
      if (next != HANDLE_INVALID) {
        handle_close(next);
      }
      return status;
    }
    parent = next;
    owned = true;
    if (!slash) {
      *result = parent;
      return CALL_OK;
    }
    path = slash + 1;
    status = CALL_BAD_REQUEST;
  }
  if (owned) {
    handle_close(parent);
  }
  return status;
}

static enum call_status snapshot_file(handle_t root, const char *path, size_t limit,
    struct config_blob *blob)
{
  const char *slash = strrchr(path, '/');
  char parent_path[64];
  if (!slash || slash == path || (size_t)(slash - path) >= sizeof(parent_path) || !slash[1]) {
    return CALL_BAD_REQUEST;
  }
  memcpy(parent_path, path, (size_t)(slash - path));
  parent_path[slash - path] = '\0';
  handle_t parent = HANDLE_INVALID, file = HANDLE_INVALID;
  enum call_status status = parent_open(root, parent_path, RECOVERY_READ, false, &parent);
  if (status == CALL_OK) {
    status = child(parent, slash + 1, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
  }
  if (parent != HANDLE_INVALID && handle_close(parent) != CALL_OK) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status != CALL_OK) {
    if (file != HANDLE_INVALID) {
      handle_close(file);
    }
    if (status == CALL_NOT_FOUND) {
      config_blob_destroy(blob);
    }
    return status;
  }
  uint64_t size = 0, final_size = 0;
  bool ok = file_size(file, &size) == CALL_OK && size <= limit;
  unsigned char *bytes = ok ? malloc((size_t)size + 1) : NULL;
  ok = ok && bytes;
  for (size_t offset = 0; ok && offset < size;) {
    size_t count = 0;
    ok = file_read(file, offset, bytes + offset, (size_t)size - offset, &count) == CALL_OK &&
        count && count <= size - offset;
    if (ok) {
      offset += count;
    }
  }
  ok = ok && file_size(file, &final_size) == CALL_OK && final_size == size &&
      config_blob_set(blob, bytes, (size_t)size);
  free(bytes);
  ok = handle_close(file) == CALL_OK && ok;
  return ok ? CALL_OK : CALL_IO;
}

static bool snapshot_matches(handle_t root, const char *path, size_t limit,
    const struct config_blob *expected)
{
  struct config_blob actual = {0};
  bool ok = snapshot_file(root, path, limit, &actual) == CALL_OK && config_blob_equal(&actual, expected);
  config_blob_destroy(&actual);
  return ok;
}

static bool fallback_absent(handle_t root)
{
  struct config_blob bytes = {0};
  enum call_status status = snapshot_file(root, RECOVERY_FALLBACK, CONFIG_FILE_MAX_BYTES, &bytes);
  config_blob_destroy(&bytes);
  return status == CALL_NOT_FOUND;
}

static bool no_efi_shadow(handle_t root)
{
  const char *const paths[] = {"EFI/BOOT/limine.conf", "EFI/pyxis/limine.conf"};
  for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    struct config_blob bytes = {0};
    enum call_status status = snapshot_file(root, paths[i], CONFIG_FILE_MAX_BYTES, &bytes);
    config_blob_destroy(&bytes);
    if (status != CALL_NOT_FOUND) return false;
  }
  return true;
}

static bool manifest_render(const struct install_recovery *recovery, struct config_blob *output)
{
  char disk[37], esp[37], text[RECOVERY_MANIFEST_MAX];
  install_guid_text(recovery->facts.disk_guid, disk);
  install_guid_text(recovery->facts.partition_guid, esp);
  int size = snprintf(text, sizeof(text), "PYXIS-RECOVERY 1\ndisk %s\nesp %s\n"
      "kernel %llu %s\narchive %llu %s\n", disk, esp,
      (unsigned long long)recovery->kernel.size, recovery->kernel.sha256,
      (unsigned long long)recovery->archive.size, recovery->archive.sha256);
  return size > 0 && (size_t)size < sizeof(text) && config_blob_set(output, text, (size_t)size);
}

static bool digest_parse(const char **cursor, const char *prefix, struct config_file_digest *digest)
{
  size_t length = strlen(prefix);
  if (strncmp(*cursor, prefix, length)) {
    return false;
  }
  const char *p = *cursor + length;
  uint64_t size = 0;
  if (*p < '0' || *p > '9') {
    return false;
  }
  while (*p >= '0' && *p <= '9') {
    if (size > (UINT64_MAX - (unsigned)(*p - '0')) / 10) {
      return false;
    }
    size = size * 10 + (unsigned)(*p++ - '0');
  }
  if (*p++ != ' ' || !size || size > RECOVERY_FILE_MAX || strlen(p) < 65) {
    return false;
  }
  *digest = (struct config_file_digest){.present = true, .size = size};
  memcpy(digest->sha256, p, 64);
  if (p[64] != '\n' || !config_payload_digest_valid(digest)) {
    return false;
  }
  *cursor = p + 65;
  return true;
}

static bool manifest_parse(struct install_recovery *recovery)
{
  char disk[37], esp[37], prefix[128];
  install_guid_text(recovery->facts.disk_guid, disk);
  install_guid_text(recovery->facts.partition_guid, esp);
  int count = snprintf(prefix, sizeof(prefix), "PYXIS-RECOVERY 1\ndisk %s\nesp %s\n", disk, esp);
  if (count <= 0 || (size_t)count >= sizeof(prefix) ||
      recovery->manifest.size <= (size_t)count ||
      memcmp(recovery->manifest.bytes, prefix, (size_t)count)) {
    return false;
  }
  const char *cursor = (char *)recovery->manifest.bytes + count;
  if (!digest_parse(&cursor, "kernel ", &recovery->kernel) ||
      !digest_parse(&cursor, "archive ", &recovery->archive) || *cursor) {
    return false;
  }
  struct config_blob canonical = {0};
  bool ok = manifest_render(recovery, &canonical) && config_blob_equal(&canonical, &recovery->manifest);
  config_blob_destroy(&canonical);
  return ok;
}

static bool fallback_render(const struct install_recovery *recovery, struct config_blob *output)
{
  char esp[37], text[768];
  install_guid_text(recovery->facts.partition_guid, esp);
  int size = snprintf(text, sizeof(text), "# PYXIS STATIC RECOVERY 1\ntimeout: no\n"
      "/Pyxis immutable rescue\n  protocol: limine\n"
      "  path: guid(%s):/EFI/pyxis/recovery/kernel.elf\n"
      "  module_path: guid(%s):/EFI/pyxis/recovery/boot.arc\n"
      "  paging_mode: 4level\n  kaslr: no\n"
      "  cmdline: " INSTALL_BOOT_INIT_OPTION " " INSTALL_RESCUE_OPTION "\n", esp, esp);
  return size > 0 && (size_t)size < sizeof(text) && config_blob_set(output, text, (size_t)size);
}

static bool frozen_evidence_valid(const struct install_recovery *recovery)
{
  struct config_blob manifest = {0}, fallback = {0};
  bool ok = recovery->kernel.present && recovery->archive.present &&
      recovery->kernel.size && recovery->archive.size &&
      recovery->kernel.size <= RECOVERY_FILE_MAX && recovery->archive.size <= RECOVERY_FILE_MAX &&
      config_payload_digest_valid(&recovery->kernel) && config_payload_digest_valid(&recovery->archive) &&
      manifest_render(recovery, &manifest) && config_blob_equal(&manifest, &recovery->manifest) &&
      fallback_render(recovery, &fallback) && config_blob_equal(&fallback, &recovery->fallback);
  config_blob_destroy(&manifest);
  config_blob_destroy(&fallback);
  return ok;
}

static bool recovery_contents(handle_t parent)
{
  unsigned count = sizeof(recovery_names) / sizeof(*recovery_names);
  unsigned seen = 0;
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply entry;
    if (directory_enumerate(parent, &cursor, name, sizeof(name), &entry) != CALL_OK) {
      return false;
    }
    if (entry.outcome == DIRECTORY_END) {
      return seen == ((1u << count) - 1);
    }
    if (entry.outcome != DIRECTORY_ENTRY || entry.kind != DIRECTORY_KIND_FILE) {
      return false;
    }
    unsigned slot = 0;
    for (; slot < count; ++slot) {
      if (name_equal(name, recovery_names[slot])) {
        break;
      }
    }
    if (slot == count || (seen & (1u << slot))) {
      return false;
    }
    seen |= 1u << slot;
    cursor = entry.cursor;
  }
}

static bool payload_digest(handle_t file, struct config_file_digest *digest)
{
  struct install_source source = {.handle = file};
  uint64_t final_size = 0;
  return file_size(file, &source.bytes) == CALL_OK && source.bytes && source.bytes <= RECOVERY_FILE_MAX &&
      install_boot_payload_digest(&source, digest) && file_size(file, &final_size) == CALL_OK &&
      final_size == source.bytes;
}

static bool verify_pair(handle_t parent, handle_t ram, const struct install_recovery *recovery)
{
  const struct config_file_digest *expected[] = {&recovery->kernel, &recovery->archive};
  for (unsigned i = 0; i < 2; ++i) {
    handle_t file = HANDLE_INVALID;
    if (child(parent, recovery_names[i], DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file) != CALL_OK) {
      return false;
    }
    bool ok = install_esp_transfer_file(file, ram, true, expected[i]->size,
        expected[i]->sha256, RECOVERY_FILE_MAX) == CALL_OK;
    ok = handle_close(file) == CALL_OK && ok;
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool prepare_sources(handle_t boot, handle_t loaded_kernel, struct install_recovery *recovery)
{
  recovery->sources[0].handle = loaded_kernel;
  handle_t share = HANDLE_INVALID, installer = HANDLE_INVALID;
  bool ok = directory_lookup(boot, "share", DIRECTORY_KIND_DIRECTORY, RECOVERY_READ, &share) == CALL_OK &&
      directory_lookup(share, "installer", DIRECTORY_KIND_DIRECTORY, RECOVERY_READ, &installer) == CALL_OK &&
      directory_lookup(installer, "recovery.arc", DIRECTORY_KIND_FILE, FILE_RIGHT_READ,
          &recovery->sources[1].handle) == CALL_OK;
  if (share != HANDLE_INVALID) {
    ok = handle_close(share) == CALL_OK && ok;
  }
  if (installer != HANDLE_INVALID) {
    ok = handle_close(installer) == CALL_OK && ok;
  }
  struct config_file_digest *digests[] = {&recovery->kernel, &recovery->archive};
  for (unsigned i = 0; ok && i < 2; ++i) {
    ok = payload_digest(recovery->sources[i].handle, digests[i]);
    if (ok) {
      recovery->sources[i].bytes = digests[i]->size;
    }
  }
  return ok && manifest_render(recovery, &recovery->manifest);
}

bool install_recovery_prepare_fresh(handle_t boot, handle_t loaded_kernel,
    const struct config_target_facts *planned, struct install_recovery *recovery)
{
  if (!recovery) {
    return false;
  }
  *recovery = (struct install_recovery){0};
  if (!planned) {
    return false;
  }
  unsigned char disk = 0, esp = 0;
  for (unsigned i = 0; i < 16; ++i) {
    disk |= planned->disk_guid[i];
    esp |= planned->partition_guid[i];
  }
  if (!disk || !esp) {
    return false;
  }
  recovery->facts = *planned;
  return prepare_sources(boot, loaded_kernel, recovery) && fallback_render(recovery, &recovery->fallback);
}

bool install_recovery_prepare(handle_t root, handle_t boot, handle_t loaded_kernel,
    handle_t workspace, const struct config_target_facts *facts, struct install_recovery *recovery)
{
  if (!recovery) {
    return false;
  }
  *recovery = (struct install_recovery){0};
  if (!facts || !config_target_valid(facts) || !root_bound(root, facts)) {
    return false;
  }
  recovery->facts = *facts;
  if (!no_efi_shadow(root)) {
    return false;
  }
  handle_t parent = HANDLE_INVALID;
  enum call_status status = parent_open(root, RECOVERY_DIRECTORY, RECOVERY_READ, false, &parent);
  bool ok;
  if (status == CALL_NOT_FOUND) {
    ok = fallback_absent(root) && prepare_sources(boot, loaded_kernel, recovery);
  } else if (status == CALL_OK) {
    handle_t ram = HANDLE_INVALID;
    recovery->present = true;
    ok = recovery_contents(parent) &&
        snapshot_file(root, RECOVERY_MANIFEST, RECOVERY_MANIFEST_MAX, &recovery->manifest) == CALL_OK &&
        manifest_parse(recovery) && install_esp_transfer_workspace(workspace, &ram) == CALL_OK &&
        verify_pair(parent, ram, recovery);
    if (ram != HANDLE_INVALID) {
      ok = handle_close(ram) == CALL_OK && ok;
    }
    ok = handle_close(parent) == CALL_OK && ok;
  } else {
    return false;
  }
  if (ok) {
    ok = fallback_render(recovery, &recovery->fallback) &&
        (!recovery->present || snapshot_matches(root, RECOVERY_FALLBACK,
            CONFIG_FILE_MAX_BYTES, &recovery->fallback));
  }
  return ok;
}

static bool file_new(handle_t parent, const char *name, const struct config_blob *bytes)
{
  handle_t file = HANDLE_INVALID;
  bool ok = directory_create(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, &file) == CALL_OK;
  for (size_t at = 0; ok && at < bytes->size;) {
    size_t count = 0;
    ok = file_write(file, at, bytes->bytes + at, bytes->size - at, &count) == CALL_OK &&
        count && count <= bytes->size - at;
    if (ok) {
      at += count;
    }
  }
  if (ok) {
    ok = file_sync(file) == CALL_OK;
  }
  if (file != HANDLE_INVALID) {
    ok = handle_close(file) == CALL_OK && ok;
  }
  return ok;
}

bool install_recovery_provision(handle_t root, handle_t control, handle_t workspace,
    const struct install_recovery *recovery)
{
  struct config_target_facts facts;
  if (!recovery || !install_snapshot_facts(control, &facts) ||
      !install_snapshot_same_target(&facts, &recovery->facts) || !root_bound(root, &facts) ||
      !frozen_evidence_valid(recovery) || !no_efi_shadow(root)) {
    return false;
  }
  handle_t parent = HANDLE_INVALID, ancestor = HANDLE_INVALID, boot = HANDLE_INVALID, ram = HANDLE_INVALID;
  bool ok = install_esp_transfer_workspace(workspace, &ram) == CALL_OK;
  if (ok && recovery->present) {
    ok = parent_open(root, RECOVERY_DIRECTORY, RECOVERY_READ, false, &parent) == CALL_OK &&
        recovery_contents(parent) && snapshot_matches(root, RECOVERY_MANIFEST,
            RECOVERY_MANIFEST_MAX, &recovery->manifest) && verify_pair(parent, ram, recovery) &&
        snapshot_matches(root, RECOVERY_FALLBACK, CONFIG_FILE_MAX_BYTES, &recovery->fallback);
  } else if (ok) {
    enum call_status status = parent_open(root, RECOVERY_DIRECTORY, RECOVERY_READ, false, &parent);
    ok = status == CALL_NOT_FOUND && fallback_absent(root);
    if (ok) {
      ok = parent_open(root, "EFI/pyxis", RECOVERY_WRITE, true, &ancestor) == CALL_OK &&
          directory_create(ancestor, "recovery", DIRECTORY_KIND_DIRECTORY, RECOVERY_WRITE, &parent) == CALL_OK &&
          directory_sync(ancestor) == CALL_OK;
    }
    const struct config_file_digest *digests[] = {&recovery->kernel, &recovery->archive};
    for (unsigned i = 0; ok && i < 2; ++i) {
      handle_t file = HANDLE_INVALID;
      ok = directory_create(parent, recovery_names[i], DIRECTORY_KIND_FILE,
          FILE_RIGHT_READ | FILE_RIGHT_WRITE, &file) == CALL_OK;
      if (ok) {
        ok = install_esp_transfer_source(&recovery->sources[i], file, ram,
            digests[i]->size, digests[i]->sha256, RECOVERY_FILE_MAX) == CALL_OK && file_sync(file) == CALL_OK;
      }
      if (file != HANDLE_INVALID) {
        ok = handle_close(file) == CALL_OK && ok;
      }
    }
    if (ok) {
      ok = directory_sync(parent) == CALL_OK && mount_sync(control) == CALL_OK &&
          verify_pair(parent, ram, recovery);
    }
    if (ok) {
      ok = file_new(parent, "ownership.record", &recovery->manifest) &&
          directory_sync(parent) == CALL_OK && mount_sync(control) == CALL_OK &&
          snapshot_matches(root, RECOVERY_MANIFEST, RECOVERY_MANIFEST_MAX, &recovery->manifest);
    }
    if (ok) {
      ok = parent_open(root, "boot", RECOVERY_WRITE, true, &boot) == CALL_OK &&
          file_new(boot, "limine.conf", &recovery->fallback) && directory_sync(boot) == CALL_OK &&
          mount_sync(control) == CALL_OK && snapshot_matches(root, RECOVERY_FALLBACK,
              CONFIG_FILE_MAX_BYTES, &recovery->fallback);
    }
  }
  if (ram != HANDLE_INVALID) {
    ok = handle_close(ram) == CALL_OK && ok;
  }
  if (boot != HANDLE_INVALID) {
    ok = handle_close(boot) == CALL_OK && ok;
  }
  if (ancestor != HANDLE_INVALID) {
    ok = handle_close(ancestor) == CALL_OK && ok;
  }
  if (parent != HANDLE_INVALID) {
    ok = handle_close(parent) == CALL_OK && ok;
  }
  return ok;
}

uint64_t install_recovery_space(const struct install_recovery *recovery, uint64_t cluster)
{
  if (!recovery || !cluster || cluster > UINT64_MAX / 8) {
    return UINT64_MAX;
  }
  if (recovery->present) {
    return 0;
  }
  uint64_t sizes[] = {recovery->kernel.size, recovery->archive.size, recovery->manifest.size, recovery->fallback.size};
  uint64_t total = 8 * cluster;
  for (unsigned i = 0; i < 4; ++i) {
    uint64_t blocks = sizes[i] / cluster + (sizes[i] % cluster != 0);
    if (blocks > (UINT64_MAX - total) / cluster) {
      return UINT64_MAX;
    }
    total += blocks * cluster;
  }
  return total;
}

void install_recovery_destroy(struct install_recovery *recovery)
{
  if (!recovery) {
    return;
  }
  if (recovery->sources[1].handle != HANDLE_INVALID) {
    handle_close(recovery->sources[1].handle);
  }
  config_blob_destroy(&recovery->manifest);
  config_blob_destroy(&recovery->fallback);
  *recovery = (struct install_recovery){0};
}

static bool recorded_menu_read(handle_t root, const struct config_recovery_file *owned,
    struct config_blob *bytes)
{
  if (!owned->digest.present) {
    return false;
  }
  char path[sizeof("boot/limine/") + CONFIG_ARTIFACT_NAME_BYTES];
  snprintf(path, sizeof(path), "boot/limine/%s", owned->name);
  return snapshot_file(root, path, CONFIG_FILE_MAX_BYTES, bytes) == CALL_OK &&
      bytes->size == owned->digest.size && !strcmp(bytes->digest.sha256, owned->digest.sha256);
}

static bool committed_menu(handle_t root, const struct config_record *record,
    const uint8_t disk_guid[16], uint64_t pool_partition, struct config_blob *menu)
{
  if (!record->committed || !record->boot.present) {
    return false;
  }
  struct config_blob current = {0}, previous = {0};
  bool ok = snapshot_file(root, "boot/limine/limine.conf", CONFIG_FILE_MAX_BYTES, &current) == CALL_OK;
  const struct install_boot_entry_paths active = {
    record->boot.kernel.path, record->boot.archive.path};
  const struct install_boot_entry_paths inherited = {
    record->previous_boot.kernel.path, record->previous_boot.archive.path};
  const struct install_boot_menu committed = {&active,
    record->previous_boot.present ? &inherited : NULL, false};
  bool adoption = false, pending = true, is_current = false;
  if (ok) {
    ok = install_boot_entry_current(&current, &committed, disk_guid, pool_partition,
        &is_current);
  }
  if (ok && is_current) {
    ok = config_blob_set(menu, current.bytes, current.size);
    pending = !ok;
  } else if (ok) {
    ok = recorded_menu_read(root, &record->previous[CONFIG_LIMINE], &previous) &&
        config_blob_equal(&current, &previous);
    /* The digest proof owns its previous entry, whatever attempt it names. */
    const struct install_boot_menu replaced = {&inherited, NULL, true};
    if (ok) {
      ok = install_boot_entry_plan(&current, record->previous_boot.present ? &replaced : NULL,
          disk_guid, pool_partition, &committed, !record->previous_boot.present, menu,
          &adoption, &pending);
    }
  }
  config_blob_destroy(&current);
  config_blob_destroy(&previous);
  return ok && !pending;
}

bool install_recovery_menu_review(handle_t root, const struct config_blob *record,
    const struct config_target_facts *facts, uint64_t pool_partition)
{
  if (!record || !facts || !config_target_valid(facts) || !root_bound(root, facts)) {
    return false;
  }
  struct config_record parsed = {0};
  if (record->digest.present && !config_record_parse(record->bytes, record->size, &parsed)) {
    return false;
  }
  handle_t parent = HANDLE_INVALID;
  enum call_status status = parent_open(root, "boot/limine", RECOVERY_READ, false, &parent);
  if (status == CALL_NOT_FOUND) {
    return true;
  }
  if (status != CALL_OK) {
    return false;
  }
  bool ok = true;
  struct directory_cursor cursor = {0};
  for (;;) {
    char name[256];
    struct directory_enumerate_reply entry;
    status = directory_enumerate(parent, &cursor, name, sizeof(name), &entry);
    if (status != CALL_OK) {
      ok = false;
      break;
    }
    if (entry.outcome == DIRECTORY_END) {
      break;
    }
    if (entry.outcome != DIRECTORY_ENTRY || entry.kind != DIRECTORY_KIND_FILE) {
      ok = false;
      break;
    }
    bool known = name_equal(name, "limine.conf");
    const struct config_recovery_file *owned[] = {
      &parsed.previous[CONFIG_LIMINE], &parsed.obsolete[CONFIG_LIMINE]
    };
    for (unsigned i = 0; !known && i < sizeof(owned) / sizeof(*owned); ++i) {
      if (!owned[i]->digest.present || !name_equal(name, owned[i]->name)) {
        continue;
      }
      struct config_blob bytes = {0};
      known = recorded_menu_read(root, owned[i], &bytes);
      config_blob_destroy(&bytes);
    }
    char staged[CONFIG_ARTIFACT_NAME_BYTES];
    if (!known && parsed.committed && parsed.boot.present &&
        config_record_artifact_name(parsed.transaction, CONFIG_LIMINE, "new", staged) &&
        name_equal(name, staged)) {
      char path[sizeof("boot/limine/") + CONFIG_ARTIFACT_NAME_BYTES];
      snprintf(path, sizeof(path), "boot/limine/%s", staged);
      struct config_blob bytes = {0}, menu = {0};
      known = snapshot_file(root, path, CONFIG_FILE_MAX_BYTES, &bytes) == CALL_OK &&
          committed_menu(root, &parsed, facts->disk_guid, pool_partition, &menu) && config_blob_equal(&bytes, &menu);
      config_blob_destroy(&bytes);
      config_blob_destroy(&menu);
    }
    if (!known) {
      ok = false;
      break;
    }
    cursor = entry.cursor;
  }
  return handle_close(parent) == CALL_OK && ok;
}

bool install_recovery_resolved(handle_t root, const struct config_blob *record)
{
  struct config_record parsed = {0};
  if (!record->digest.present) return true;
  if (!config_record_parse(record->bytes, record->size, &parsed) || !parsed.committed) return false;
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!parsed.obsolete[i].digest.present) continue;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", i == CONFIG_LIMINE ? "boot/limine" : "EFI/pyxis/config",
        parsed.obsolete[i].name);
    struct config_blob artifact = {0};
    bool ok = install_snapshot_file(root, path, CONFIG_FILE_MAX_BYTES, &artifact);
    bool present = artifact.digest.present;
    config_blob_destroy(&artifact);
    if (!ok || present) {
      fputs("installer: retained recovery needs a fresh pending completion Review or manual disposition\n", stderr);
      return false;
    }
  }
  char staged[CONFIG_ARTIFACT_NAME_BYTES], path[256];
  if (!config_record_artifact_name(parsed.transaction, CONFIG_LIMINE, "new", staged)) return false;
  snprintf(path, sizeof(path), "boot/limine/%s", staged);
  struct config_blob artifact = {0};
  bool ok = install_snapshot_file(root, path, CONFIG_FILE_MAX_BYTES, &artifact);
  bool present = artifact.digest.present;
  config_blob_destroy(&artifact);
  if (!ok || present) {
    fputs("installer: retained recovery needs a fresh pending completion Review or manual disposition\n", stderr);
    return false;
  }
  return true;
}
