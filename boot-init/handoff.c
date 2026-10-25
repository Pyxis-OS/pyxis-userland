#include "handoff.h"
#include "../installer/boot_record.h"
#include "../installer/digest.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <path.h>
#include <stdio.h>
#include <string.h>

#define READER_ROOT_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define HANDOFF_PARENT_RIGHTS (READER_ROOT_RIGHTS | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE | DIRECTORY_RIGHT_FILESYSTEM_INFO)
#define HANDOFF_PARENT_NAME ".config-handoff"
#define EVIDENCE_NAME "boot-evidence"
#define EVIDENCE_BYTES 512u

static bool loaded_digest(handle_t file, struct config_file_digest *result)
{
  struct handle_info grant;
  struct file_info_reply initial, final;
  uint64_t required = FILE_INFO_SIZE_VALID | FILE_INFO_DOMAIN_VALID | FILE_INFO_OBJECT_VALID;
  if (file == HANDLE_INVALID || handle_query(file, &grant) != CALL_OK ||
      grant.kind != HANDLE_KIND_NATIVE || grant.protocol != PROTOCOL_FILE ||
      grant.rights != FILE_RIGHT_READ || grant.transport || file_info(file, &initial) != CALL_OK ||
      (initial.valid & required) != required || !initial.size) {
    return false;
  }
  struct install_digest digest = {0};
  bool ok = install_digest_begin(&digest);
  unsigned char bytes[FILE_READ_MAX_BYTES];
  for (uint64_t offset = 0; ok && offset < initial.size;) {
    size_t wanted = initial.size - offset > sizeof(bytes) ? sizeof(bytes) :
        (size_t)(initial.size - offset);
    size_t count = 0;
    ok = file_read(file, offset, bytes, wanted, &count) == CALL_OK && count && count <= wanted &&
        install_digest_update(&digest, bytes, count);
    if (ok) {
      offset += count;
    }
  }
  struct config_file_digest loaded = {.present = true, .size = initial.size};
  ok = ok && file_info(file, &final) == CALL_OK && (final.valid & required) == required &&
      initial.size == final.size && initial.domain == final.domain && initial.object == final.object &&
      install_digest_finish(&digest, loaded.sha256);
  install_digest_abort(&digest);
  if (ok) {
    *result = loaded;
  }
  return ok;
}

static bool read_snapshot(handle_t parent, const char *path, size_t limit,
    struct config_handoff_bytes *bytes)
{
  struct path_context context = {.directories = &parent, .count = 1};
  handle_t directories[4];
  char component[256];
  struct path_workspace workspace = {
    .directories = directories, .directory_capacity = 4,
    .component = component, .component_capacity = sizeof(component),
  };
  handle_t file = HANDLE_INVALID;
  enum call_status status = path_resolve_native(&context, path, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &workspace, &file);
  if (status != CALL_OK) {
    return false;
  }
  uint64_t size = 0;
  bool ok = file_size(file, &size) == CALL_OK && size <= limit &&
      config_handoff_read(file, bytes, &status) && bytes->size <= limit;
  if (handle_close(file)) {
    ok = false;
  }
  return ok;
}

/* The committed record, read through one early ESP reader that is retired
 * before returning. RECORD stays empty without a committed one. */
static bool read_record(handle_t esp_mount, struct config_handoff_bytes *record)
{
  if (esp_mount == HANDLE_INVALID) {
    return true;
  }
  struct mount_fat32_reply reader = {0};
  if (mount_open_fat32(esp_mount, READER_ROOT_RIGHTS, &reader) != CALL_OK) {
    return true;
  }
  struct config_record parsed = {0};
  if (!read_snapshot(reader.root, "EFI/pyxis/config/" CONFIG_RECORD_NAME,
      CONFIG_RECORD_MAX_BYTES, record) ||
      !config_record_parse(record->bytes, record->size, &parsed) || !parsed.committed) {
    config_handoff_bytes_free(record);
  }
  bool drained = handle_close(reader.root) == 0;
  enum call_status status = mount_retire(reader.control);
  bool closed = handle_close(reader.control) == 0;
  if (!drained || status != CALL_OK || !closed) {
    fprintf(stderr, "boot-init: early ESP reader retirement failed (status %u)\n", status);
    return false;
  }
  return true;
}

/* NAME..., one handoff FILE each, in a private RAM parent removed before return. */
static bool handoff_files(handle_t ram, size_t count, const char *const names[],
    const char *const bytes[], const size_t sizes[], handle_t files[], enum call_status *status)
{
  handle_t parent = HANDLE_INVALID;
  *status = directory_create(ram, HANDOFF_PARENT_NAME, DIRECTORY_KIND_DIRECTORY,
      HANDOFF_PARENT_RIGHTS, &parent);
  if (*status != CALL_OK) {
    return false;
  }
  bool ok = true;
  for (size_t i = 0; ok && i < count; ++i) {
    ok = config_handoff_file(parent, names[i], bytes[i], sizes[i], &files[i], status);
  }
  enum call_status removed = directory_remove(ram, HANDOFF_PARENT_NAME, DIRECTORY_KIND_DIRECTORY);
  bool closed = handle_close(parent) == 0;
  if (ok && (removed != CALL_OK || !closed)) {
    *status = removed != CALL_OK ? removed : CALL_IO;
    ok = false;
  }
  for (size_t i = 0; !ok && i < count; ++i) {
    if (files[i] != HANDLE_INVALID) {
      handle_close(files[i]);
      files[i] = HANDLE_INVALID;
    }
  }
  return ok;
}

bool boot_evidence_prepare(const struct boot_evidence_authority *authority, handle_t *evidence)
{
  *evidence = HANDLE_INVALID;
  struct config_handoff_bytes record = {0};
  if (!read_record(authority->esp_mount, &record)) {
    return false;
  }
  struct config_file_digest kernel = {0}, archive = {0}, digest = {0};
  bool valid = record.bytes && authority->pool;
  if (valid) {
    struct install_digest hash = {0};
    valid = loaded_digest(authority->loaded_kernel, &kernel) &&
        loaded_digest(authority->loaded_archive, &archive) && install_digest_begin(&hash) &&
        install_digest_update(&hash, record.bytes, record.size) &&
        install_digest_finish(&hash, digest.sha256);
    install_digest_abort(&hash);
  }
  digest.present = true;
  digest.size = record.size;
  char text[EVIDENCE_BYTES];
  valid = valid && snprintf(text, sizeof(text),
      "PYXIS-BOOT-EVIDENCE 1\nkernel %llu %s\narchive %llu %s\nrecord %llu %s\npool %u\n",
      (unsigned long long)kernel.size, kernel.sha256,
      (unsigned long long)archive.size, archive.sha256,
      (unsigned long long)digest.size, digest.sha256, authority->pool) < (int)sizeof(text);
  config_handoff_bytes_free(&record);
  /* Only an upgrade needs the evidence; boot continues without it. */
  if (!valid) {
    return true;
  }
  enum call_status status;
  const char *names[] = {EVIDENCE_NAME};
  const char *bytes[] = {text};
  size_t sizes[] = {strlen(text)};
  if (!handoff_files(authority->ram, 1, names, bytes, sizes, evidence, &status)) {
    printf("boot-init: boot evidence unavailable (status %u)\n", status);
  }
  return true;
}

bool boot_configuration_handoff(handle_t ram, const struct config_handoff_bytes *session,
    const struct config_handoff_bytes *network, struct config_handoff *handoff,
    enum call_status *status)
{
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    handoff->files[i] = HANDLE_INVALID;
  }
  handle_t files[2] = {HANDLE_INVALID, HANDLE_INVALID};
  const char *names[] = {"session.lua", "network.lua"};
  const char *bytes[] = {session->bytes, network->bytes};
  size_t sizes[] = {session->size, network->size};
  if (!handoff_files(ram, 2, names, bytes, sizes, files, status)) {
    return false;
  }
  handoff->files[CONFIG_LUA_SESSION] = files[0];
  handoff->files[CONFIG_LUA_NETWORK] = files[1];
  return true;
}
