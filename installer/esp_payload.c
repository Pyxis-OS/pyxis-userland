#include "esp_payload.h"
#include "esp_directory.h"
#include "esp_transfer.h"
#include "digest.h"
#include "progress.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES)
#define BOOT_DIRECTORY_WRITE (BOOT_DIRECTORY_READ | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES)
#define BOOT_CONTROL_READ (MOUNT_RIGHT_OPEN_ROOT | MOUNT_RIGHT_OBSERVE)
#define BOOT_CONTROL_WRITE (BOOT_CONTROL_READ | MOUNT_RIGHT_WRITE)
#define BOOT_MANIFEST_BYTES 2048u

static const char *const payload_names[INSTALL_ESP_PAYLOAD_COUNT] = {
  "kernel.elf", "boot.arc", "revision.txt", "entry.txt"
};

struct install_esp_payloads {
  handle_t root, control; /* Borrowed. */
  handle_t parent, files[INSTALL_ESP_PAYLOAD_COUNT], manifest, workspace;
  struct mount_state_reply expected;
  struct install_esp_payload_record record;
  char manifest_bytes[BOOT_MANIFEST_BYTES];
  bool synchronized;
};

static bool fail(struct install_esp_payload_report *report, enum call_status status)
{
  report->status = status;
  install_progress_finish();
  return false;
}

static bool grant(handle_t handle, uint64_t protocol, uint64_t rights,
    struct install_esp_payload_report *report)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return fail(report, status);
  }
  return (info.kind == HANDLE_KIND_NATIVE && !info.transport && info.protocol == protocol &&
      (info.rights & rights) == rights) || fail(report, CALL_DENIED);
}

static bool same_object(handle_t held, handle_t current, bool directory,
    uint64_t domain, struct install_esp_payload_report *report)
{
  struct file_info_reply original, reopened;
  enum call_status status = directory ? directory_info(held, &original) : file_info(held, &original);
  if (status == CALL_OK) {
    status = directory ? directory_info(current, &reopened) : file_info(current, &reopened);
  }
  if (status != CALL_OK) {
    return fail(report, status);
  }
  return ((original.valid & FILE_INFO_OBJECT_VALID) &&
      (reopened.valid & FILE_INFO_OBJECT_VALID) && original.domain == domain &&
      reopened.domain == domain && original.object == reopened.object) || fail(report, CALL_DENIED);
}

static bool hex_valid(const char *text, size_t size)
{
  if (!text || strnlen(text, size + 1) != size) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

static bool revision_valid(const char *revision)
{
  if (!revision) {
    return false;
  }
  size_t size = strnlen(revision, 64);
  if (!size || size >= 64 || !strcmp(revision, ".") || !strcmp(revision, "..")) {
    return false;
  }
  /* A portable FAT component, never the stable configuration namespace. */
  char lower[64];
  for (size_t i = 0; i < size; ++i) {
    unsigned char ch = (unsigned char)revision[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) {
      return false;
    }
    lower[i] = ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch;
  }
  lower[size] = '\0';
  return revision[size - 1] != '.' && strcmp(lower, "config");
}

bool install_esp_payload_path(const char *revision, const char *attempt,
    enum install_esp_payload_index index, char path[INSTALL_ESP_PAYLOAD_PATH_BYTES])
{
  if (!path || !revision_valid(revision) || !hex_valid(attempt, 32) ||
      (unsigned)index > INSTALL_ESP_OWNERSHIP) {
    return false;
  }
  const char *name = index == INSTALL_ESP_OWNERSHIP ? INSTALL_ESP_PAYLOAD_MANIFEST : payload_names[index];
  int count = snprintf(path, INSTALL_ESP_PAYLOAD_PATH_BYTES, "EFI/pyxis/%s/%s/%s", revision, attempt, name);
  return count > 0 && count < (int)INSTALL_ESP_PAYLOAD_PATH_BYTES;
}

static unsigned payload_count(const struct install_esp_payloads *result)
{
  return result->record.entry_present ? INSTALL_ESP_PAYLOAD_COUNT : INSTALL_ESP_ENTRY;
}

static bool current_target(const struct install_esp_payloads *result,
    struct install_esp_payload_report *report)
{
  if (!grant(result->control, PROTOCOL_MOUNT, BOOT_CONTROL_READ, report)) {
    return false;
  }
  struct mount_state_reply current;
  enum call_status status = mount_observe(result->control, &current);
  if (status != CALL_OK) {
    return fail(report, status);
  }
  const struct mount_state_reply *expected = &result->expected;
  if (!(expected->flags & MOUNT_STATE_IDENTITY_VALID) ||
      !(current.flags & MOUNT_STATE_IDENTITY_VALID) ||
      !(current.flags & MOUNT_STATE_READ_WRITE) ||
      (current.flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED |
        MOUNT_STATE_CLEANUP_PENDING | MOUNT_STATE_ALLOCATION_INVALID)) ||
      current.domain != expected->domain || current.device != expected->device ||
      current.partition != expected->partition || current.first_block != expected->first_block ||
      current.block_count != expected->block_count ||
      current.device_block_size != expected->device_block_size ||
      current.device_block_count != expected->device_block_count ||
      current.partition_attributes != expected->partition_attributes ||
      memcmp(current.disk_guid, expected->disk_guid, sizeof(current.disk_guid)) ||
      memcmp(current.partition_guid, expected->partition_guid, sizeof(current.partition_guid)) ||
      memcmp(current.partition_type, expected->partition_type, sizeof(current.partition_type))) {
    return fail(report, CALL_DENIED);
  }
  handle_t root = HANDLE_INVALID;
  status = mount_reopen_fat32(result->control, BOOT_DIRECTORY_READ, &root);
  bool ok = status == CALL_OK || fail(report, status);
  if (ok) {
    ok = same_object(result->root, root, true, current.domain, report);
  }
  if (root != HANDLE_INVALID) {
    handle_close(root);
  }
  return ok;
}

static bool directory_child(handle_t parent, const char *name, bool create,
    handle_t *child, struct install_esp_payload_report *report)
{
  uint64_t rights = create ? BOOT_DIRECTORY_WRITE : BOOT_DIRECTORY_READ;
  enum call_status status = install_esp_child_open(parent, name,
      DIRECTORY_KIND_DIRECTORY, rights, create, child);
  return status == CALL_OK || fail(report, status);
}

static bool open_attempt(const struct install_esp_payloads *result, bool create,
    handle_t *attempt, struct install_esp_payload_report *report)
{
  handle_t efi = HANDLE_INVALID, pyxis = HANDLE_INVALID, revision = HANDLE_INVALID;
  bool ok = directory_child(result->root, "EFI", create, &efi, report) &&
      directory_child(efi, "pyxis", create, &pyxis, report) &&
      directory_child(pyxis, result->record.revision, create, &revision, report);
  if (ok) {
    enum call_status status = create ? directory_create(revision, result->record.attempt,
        DIRECTORY_KIND_DIRECTORY, BOOT_DIRECTORY_WRITE, attempt) :
        install_esp_child_open(revision, result->record.attempt,
        DIRECTORY_KIND_DIRECTORY, BOOT_DIRECTORY_READ, false, attempt);
    ok = status == CALL_OK || fail(report, status);
  }
  if (revision != HANDLE_INVALID) {
    handle_close(revision);
  }
  if (pyxis != HANDLE_INVALID) {
    handle_close(pyxis);
  }
  if (efi != HANDLE_INVALID) {
    handle_close(efi);
  }
  return ok;
}

static bool manifest_format(struct install_esp_payloads *result)
{
  struct install_esp_payload_record *record = &result->record;
  int count = snprintf(result->manifest_bytes, sizeof(result->manifest_bytes),
      "PYXIS-BOOT-OWNERSHIP 1\nrevision %s\nattempt %s\n", record->revision, record->attempt);
  size_t used = count > 0 ? (size_t)count : sizeof(result->manifest_bytes);
  for (unsigned i = 0; i < payload_count(result) && used < sizeof(result->manifest_bytes); ++i) {
    if (!install_esp_payload_path(record->revision, record->attempt, i, record->paths[i])) {
      return false;
    }
    count = snprintf(result->manifest_bytes + used, sizeof(result->manifest_bytes) - used,
        "%s %llu %s\n", record->paths[i], (unsigned long long)record->payloads[i].size,
        record->payloads[i].sha256);
    used += count > 0 ? (size_t)count : sizeof(result->manifest_bytes);
  }
  if (used >= sizeof(result->manifest_bytes)) {
    return false;
  }
  if (!install_esp_payload_path(record->revision, record->attempt,
      INSTALL_ESP_OWNERSHIP, record->manifest_path)) {
    return false;
  }
  record->manifest.size = used;
  return true;
}

static bool manifest_prepare(struct install_esp_payloads *result,
    struct install_esp_payload_report *report)
{
  if (!manifest_format(result)) {
    return fail(report, CALL_LIMIT);
  }
  struct install_digest hash = {0};
  bool ok = install_digest_begin(&hash) &&
      install_digest_update(&hash, result->manifest_bytes, result->record.manifest.size) &&
      install_digest_finish(&hash, result->record.manifest.sha256);
  install_digest_abort(&hash);
  return ok || fail(report, CALL_NO_MEMORY);
}

bool install_esp_payload_space(const struct install_esp_payload_request *request,
    uint64_t cluster_bytes, uint64_t *required_clusters)
{
  if (!request || !required_clusters || cluster_bytes < 512 ||
      (cluster_bytes & (cluster_bytes - 1)) ||
      !revision_valid(request->revision) || !hex_valid(request->attempt, 32)) {
    return false;
  }
  struct install_esp_payloads planned = {0};
  struct install_esp_payload_record *record = &planned.record;
  strcpy(record->revision, request->revision);
  strcpy(record->attempt, request->attempt);
  record->entry_present = request->entry_present;
  /* EFI/pyxis/revision/attempt: each new directory fits one minimum-size
   * cluster, including dots, its child or the five payload/record entries and
   * the end marker. Each insertion can also extend its parent by one cluster.
   * This follows the validated component lengths and fixed payload names. */
  uint64_t clusters = 8;
  for (unsigned i = 0; i < payload_count(&planned); ++i) {
    const struct install_esp_payload_digest *digest = &request->expected_payloads[i];
    if (!digest->size || digest->size > INSTALL_ESP_PAYLOAD_MAX_BYTES ||
        !hex_valid(digest->sha256, 64) || request->sources[i].bytes != digest->size) {
      return false;
    }
    record->payloads[i] = *digest;
    clusters += digest->size / cluster_bytes + (digest->size % cluster_bytes != 0);
  }
  if (!manifest_format(&planned)) {
    return false;
  }
  clusters += record->manifest.size / cluster_bytes + (record->manifest.size % cluster_bytes != 0);
  if (clusters > UINT64_MAX / cluster_bytes) {
    return false;
  }
  *required_clusters = clusters;
  return true;
}

static bool verify_staged(const struct install_esp_payloads *result,
    struct install_esp_payload_report *report)
{
  report->phase = INSTALL_ESP_PAYLOAD_VERIFY;
  handle_t parent = HANDLE_INVALID;
  bool ok = current_target(result, report) &&
      open_attempt(result, false, &parent, report) &&
      same_object(result->parent, parent, true, result->expected.domain, report);
  for (unsigned i = 0; ok && i <= payload_count(result); ++i) {
    bool manifest = i == payload_count(result);
    const char *name = manifest ? INSTALL_ESP_PAYLOAD_MANIFEST : payload_names[i];
    const char *path = manifest ? result->record.manifest_path : result->record.paths[i];
    const struct install_esp_payload_digest *digest = manifest ?
      &result->record.manifest : &result->record.payloads[i];
    snprintf(report->artifact, sizeof(report->artifact), "%s", path);
    handle_t file = HANDLE_INVALID;
    enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &file);
    ok = status == CALL_OK || fail(report, status);
    if (ok) {
      ok = same_object(manifest ? result->manifest : result->files[i], file, false,
          result->expected.domain, report);
    }
    if (ok) {
      status = install_esp_transfer_file(file, result->workspace, true, digest->size,
          digest->sha256, INSTALL_ESP_PAYLOAD_MAX_BYTES);
      ok = status == CALL_OK || fail(report, status);
    }
    if (file != HANDLE_INVALID) {
      handle_close(file);
    }
  }
  if (parent != HANDLE_INVALID) {
    handle_close(parent);
  }
  return ok && current_target(result, report);
}

bool install_esp_payload_verify(const struct install_esp_payloads *result,
    struct install_esp_payload_report *report)
{
  if (!report) {
    return false;
  }
  *report = (struct install_esp_payload_report){.phase = INSTALL_ESP_PAYLOAD_VERIFY};
  if (!result || !result->synchronized) {
    return fail(report, CALL_BAD_REQUEST);
  }
  return verify_staged(result, report);
}

bool install_esp_payload_verify_file(const struct install_esp_payloads *result,
    handle_t source, const struct install_esp_payload_digest *expected,
    struct install_esp_payload_report *report)
{
  if (!report) {
    return false;
  }
  *report = (struct install_esp_payload_report){.phase = INSTALL_ESP_PAYLOAD_VERIFY};
  if (!result || !result->synchronized || source == HANDLE_INVALID || !expected ||
      expected->size > INSTALL_ESP_PAYLOAD_MAX_BYTES || !hex_valid(expected->sha256, 64)) {
    return fail(report, CALL_BAD_REQUEST);
  }
  return current_target(result, report) && grant(source, PROTOCOL_FILE, FILE_RIGHT_READ, report) &&
      transfer_file(source, result->workspace, true, expected, report) && current_target(result, report);
}

const struct install_esp_payload_record *install_esp_payload_record(
    const struct install_esp_payloads *result)
{
  return result ? &result->record : NULL;
}

static bool close_owned(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool valid = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return valid;
}

bool install_esp_payload_close(struct install_esp_payloads *result)
{
  if (!result) {
    return true;
  }
  bool complete = true;
  for (unsigned i = 0; i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
    complete = close_owned(&result->files[i]) && complete;
  }
  complete = close_owned(&result->manifest) && complete;
  complete = close_owned(&result->parent) && complete;
  complete = close_owned(&result->workspace) && complete;
  free(result);
  return complete;
}

bool install_esp_payload_stage(const struct install_esp_payload_request *request,
    struct install_esp_payloads **output, struct install_esp_payload_report *report)
{
  if (!report || !output) {
    return false;
  }
  *report = (struct install_esp_payload_report){.phase = INSTALL_ESP_PAYLOAD_CHECK};
  if (*output || !request || !revision_valid(request->revision) || !hex_valid(request->attempt, 32)) {
    return fail(report, CALL_BAD_REQUEST);
  }
  struct install_esp_payloads *result = calloc(1, sizeof(*result));
  if (!result) {
    return fail(report, CALL_NO_MEMORY);
  }
  result->root = request->root;
  result->control = request->control;
  result->expected = request->expected;
  result->parent = result->manifest = result->workspace = HANDLE_INVALID;
  for (unsigned i = 0; i < INSTALL_ESP_PAYLOAD_COUNT; ++i) {
    result->files[i] = HANDLE_INVALID;
  }
  *output = result;
  struct install_esp_payload_record *record = &result->record;
  strcpy(record->revision, request->revision);
  strcpy(record->attempt, request->attempt);
  record->entry_present = request->entry_present;
  if (!grant(result->root, PROTOCOL_DIRECTORY, BOOT_DIRECTORY_WRITE, report) ||
      !grant(result->control, PROTOCOL_MOUNT, BOOT_CONTROL_WRITE, report) ||
      !current_target(result, report)) {
    return false;
  }
  for (unsigned i = 0; i < payload_count(result); ++i) {
    const struct install_source *source = &request->sources[i];
    const struct install_esp_payload_digest *digest = &request->expected_payloads[i];
    if (!digest->size || digest->size > INSTALL_ESP_PAYLOAD_MAX_BYTES ||
        !hex_valid(digest->sha256, 64) || source->bytes != digest->size) {
      return fail(report, CALL_BAD_REQUEST);
    }
    record->payloads[i] = *digest;
    snprintf(report->artifact, sizeof(report->artifact), "%s", payload_names[i]);
    if (!source->memory && !grant(source->handle, PROTOCOL_FILE, FILE_RIGHT_READ, report)) {
      return false;
    }
    enum call_status status = install_esp_transfer_stream(source, HANDLE_INVALID, digest->size,
        digest->sha256, INSTALL_ESP_PAYLOAD_MAX_BYTES);
    if (status != CALL_OK) {
      return fail(report, status);
    }
  }
  if (!manifest_prepare(result, report)) {
    return false;
  }
  enum call_status status = install_esp_transfer_workspace(request->workspace, &result->workspace);
  if (status != CALL_OK) {
    return fail(report, status);
  }
  if (!current_target(result, report)) {
    return false;
  }
  report->phase = INSTALL_ESP_PAYLOAD_STAGE;
  snprintf(report->artifact, sizeof(report->artifact), "EFI/pyxis/%s/%s",
      record->revision, record->attempt);
  if (!open_attempt(result, true, &result->parent, report)) {
    return false;
  }
  record->attempt_created = true;
  for (unsigned i = 0; i <= payload_count(result); ++i) {
    bool manifest = i == payload_count(result);
    const char *name = manifest ? INSTALL_ESP_PAYLOAD_MANIFEST : payload_names[i];
    const char *path = manifest ? record->manifest_path : record->paths[i];
    const struct install_esp_payload_digest *digest = manifest ? &record->manifest : &record->payloads[i];
    struct install_source manifest_source = {.handle = HANDLE_INVALID,
        .bytes = record->manifest.size, .memory = (uint8_t *)result->manifest_bytes};
    const struct install_source *source = manifest ? &manifest_source : &request->sources[i];
    handle_t *file = manifest ? &result->manifest : &result->files[i];
    snprintf(report->artifact, sizeof(report->artifact), "%s", path);
    install_progress_begin("Creating owned boot payload", INSTALL_PROGRESS_NONE, NULL);
    enum call_status status = directory_create(result->parent, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, file);
    if (status != CALL_OK) {
      return fail(report, status);
    }
    status = install_esp_transfer_source(source, *file, result->workspace, digest->size,
        digest->sha256, INSTALL_ESP_PAYLOAD_MAX_BYTES);
    if (status != CALL_OK) {
      return fail(report, status);
    }
    report->phase = INSTALL_ESP_PAYLOAD_SYNC;
    install_progress_begin("Syncing staged boot payload", INSTALL_PROGRESS_NONE, NULL);
    status = file_sync(*file);
    install_progress_finish();
    if (status != CALL_OK) {
      return fail(report, status);
    }
    report->phase = INSTALL_ESP_PAYLOAD_STAGE;
  }
  report->phase = INSTALL_ESP_PAYLOAD_SYNC;
  install_progress_begin("Syncing boot payload volume", INSTALL_PROGRESS_NONE, NULL);
  status = mount_sync(result->control);
  install_progress_finish();
  if (status != CALL_OK) {
    return fail(report, status);
  }
  result->synchronized = true;
  if (!verify_staged(result, report)) {
    return false;
  }
  record->staged_verified = true;
  report->phase = INSTALL_ESP_PAYLOAD_DONE;
  return true;
}
