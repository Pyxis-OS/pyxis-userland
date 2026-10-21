#include "selection.h"
#include "../config-editor/readiness.h"
#include "../installer/digest.h"
#include "../xfer/hash.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <path.h>
#include <stdio.h>
#include <string.h>
#include <system_info.h>

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

static struct config_bytes borrowed(const struct config_handoff_bytes *bytes)
{
  return (struct config_bytes){bytes->bytes, bytes->size};
}

static bool read_esp(const struct boot_selection_authority *authority,
    struct config_handoff_bytes *completion,
    struct config_handoff_bytes files[CONFIG_SET_FILES], bool *provenance_valid)
{
  *provenance_valid = false;
  if (authority->esp_mount == HANDLE_INVALID) {
    return true;
  }
  struct mount_fat32_reply reader = {0};
  enum call_status status = mount_open_fat32(authority->esp_mount, READER_ROOT_RIGHTS, &reader);
  if (status != CALL_OK) {
    return true;
  }
  struct config_record record = {0};
  bool complete = read_snapshot(reader.root, "EFI/pyxis/config/" CONFIG_RECORD_NAME,
      CONFIG_RECORD_MAX_BYTES, completion) &&
      config_record_parse(completion->bytes, completion->size, &record) && record.committed;
  if (complete && record.boot.present && record.boot.source == CONFIG_ACTIVE_ESP) {
    for (size_t i = 0; complete && i < CONFIG_SET_FILES; ++i) {
      char path[64];
      snprintf(path, sizeof(path), "EFI/pyxis/config/%s", config_storage_file_name(i));
      complete = read_snapshot(reader.root, path, CONFIG_FILE_MAX_BYTES, &files[i]);
    }
  }
  struct config_handoff_bytes again = {0};
  if (complete) {
    complete = read_snapshot(reader.root, "EFI/pyxis/config/" CONFIG_RECORD_NAME,
        CONFIG_RECORD_MAX_BYTES, &again) && again.size == completion->size &&
        !memcmp(again.bytes, completion->bytes, again.size);
  }
  config_handoff_bytes_free(&again);
  bool drained = handle_close(reader.root) == 0;
  status = mount_retire(reader.control);
  bool closed = handle_close(reader.control) == 0;
  if (!drained || status != CALL_OK || !closed) {
    fprintf(stderr, "boot-init: early ESP reader retirement failed (status %u)\n", status);
    return false;
  }
  *provenance_valid = complete;
  return true;
}

static bool make_handoff(handle_t ram, const struct config_selection *selection,
    struct config_handoff *handoff, const char *evidence_text, handle_t *evidence)
{
  handle_t parent = HANDLE_INVALID;
  enum call_status status = directory_create(ram, HANDOFF_PARENT_NAME,
      DIRECTORY_KIND_DIRECTORY, HANDOFF_PARENT_RIGHTS, &parent);
  if (status != CALL_OK) {
    return false;
  }
  bool ok = config_handoff_create(parent, selection, handoff, &status);
  /* Only an upgrade needs the evidence; boot continues without it. */
  if (ok && evidence_text && !config_handoff_file(parent, EVIDENCE_NAME, evidence_text,
      strlen(evidence_text), evidence, &status)) {
    printf("boot-init: boot evidence unavailable (status %u)\n", status);
  }
  enum call_status removed = directory_remove(ram, HANDOFF_PARENT_NAME, DIRECTORY_KIND_DIRECTORY);
  bool closed = handle_close(parent) == 0;
  if (!ok || removed != CALL_OK || !closed) {
    config_handoff_close(handoff);
    return false;
  }
  return true;
}

bool boot_selection_prepare(const struct boot_selection_authority *authority,
    bool rescue, struct config_selection *selection, struct config_handoff *handoff,
    handle_t *evidence)
{
  static const char *const archive_paths[CONFIG_LUA_COUNT] = {
    "config/installed.lua", "config/session.lua", "config/network.lua"
  };
  struct config_handoff_bytes defaults[CONFIG_LUA_COUNT] = {0};
  struct config_handoff_bytes archive_catalog = {0}, completion = {0};
  struct config_handoff_bytes files[CONFIG_SET_FILES] = {0};
  struct config_bytes archive[CONFIG_LUA_COUNT] = {0}, esp[CONFIG_LUA_COUNT] = {0};
  bool ok = true;
  for (size_t i = 0; ok && i < CONFIG_LUA_COUNT; ++i) {
    ok = read_snapshot(authority->boot, archive_paths[i], CONFIG_FILE_MAX_BYTES, &defaults[i]);
    archive[i] = borrowed(&defaults[i]);
  }
  bool provenance_valid = false;
  bool hashing_attempted = false;
  struct config_file_digest loaded_kernel = {0}, loaded_archive = {0}, loaded_record = {0};
  if (ok && !rescue) {
    read_snapshot(authority->boot, "config/catalog.lua", CONFIG_FILE_MAX_BYTES, &archive_catalog);
    ok = read_esp(authority, &completion, files, &provenance_valid);
    if (ok && provenance_valid) {
      hashing_attempted = true;
      provenance_valid = hash_init(authority->clock, authority->random) &&
          loaded_digest(authority->loaded_kernel, &loaded_kernel) &&
          loaded_digest(authority->loaded_archive, &loaded_archive);
      loaded_record = (struct config_file_digest){.present = true, .size = completion.size};
      struct install_digest digest = {0};
      provenance_valid = provenance_valid && install_digest_begin(&digest) &&
          install_digest_update(&digest, completion.bytes, completion.size) &&
          install_digest_finish(&digest, loaded_record.sha256);
      install_digest_abort(&digest);
    }
  }
  /* What this boot actually loaded and read, for a later upgrade's identity check.
   * It attests nothing about the selection made from it. */
  char evidence_text[EVIDENCE_BYTES];
  bool evidence_valid = provenance_valid && snprintf(evidence_text, sizeof(evidence_text),
      "PYXIS-BOOT-EVIDENCE 1\nkernel %llu %s\narchive %llu %s\nrecord %llu %s\n",
      (unsigned long long)loaded_kernel.size, loaded_kernel.sha256,
      (unsigned long long)loaded_archive.size, loaded_archive.sha256,
      (unsigned long long)loaded_record.size, loaded_record.sha256) < (int)sizeof(evidence_text);
  struct system_info_identity identity = {0};
  if (system_info_get_identity(authority->system_info, &identity) != CALL_OK) {
    provenance_valid = false;
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    esp[i] = borrowed(&files[i]);
  }
  struct config_bytes trusted_catalog = borrowed(&archive_catalog);
  struct config_bytes record = borrowed(&completion), esp_catalog = borrowed(&files[CONFIG_CATALOG]);
  const char *reason = "archive defaults unavailable";
  if (ok) {
    /* Installed programs are selected by the running kernel's revision, just
     * as bind_bin does; the ESP record cannot choose another program directory. */
    ok = config_readiness_choose(archive, &trusted_catalog, identity.build_revision,
        identity.build_revision, &loaded_kernel, &loaded_archive, &record, esp, &esp_catalog,
        provenance_valid, rescue, selection, &reason);
  }
  if (hashing_attempted) {
    hash_close();
  }
  for (size_t i = 0; i < CONFIG_LUA_COUNT; ++i) {
    config_handoff_bytes_free(&defaults[i]);
  }
  for (size_t i = 0; i < CONFIG_SET_FILES; ++i) {
    config_handoff_bytes_free(&files[i]);
  }
  config_handoff_bytes_free(&archive_catalog);
  config_handoff_bytes_free(&completion);
  if (ok) {
    printf("boot-init: %s\n", reason);
    ok = make_handoff(authority->ram, selection, handoff,
        evidence_valid ? evidence_text : NULL, evidence);
  }
  if (!ok) {
    config_selection_free(selection);
    fputs("boot-init: cannot freeze installed configuration; no spaces started\n", stderr);
  }
  return ok;
}
