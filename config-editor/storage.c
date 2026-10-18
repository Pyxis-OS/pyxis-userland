#include "storage.h"
#include "../installer/digest.h"
#include "../installer/progress.h"
#include "../installer/publication.h"
#include "../installer/pending.h"
#include "../installer/pending_archive.h"

#include <clock.h>
#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PARENT_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)
#define CONFIG_PARENT_WRITE (CONFIG_PARENT_READ | DIRECTORY_RIGHT_CREATE | \
    DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define CONFIG_CONTROL_WRITE (MOUNT_RIGHT_OBSERVE | MOUNT_RIGHT_WRITE | MOUNT_RIGHT_INTERVAL)
#define CONFIG_IO_BYTES 4096u

static bool failed(struct config_storage_report *report, enum call_status status)
{
  install_progress_pause();
  install_progress_finish();
  if (report) {
    report->status = status;
  }
  return false;
}

void config_blob_destroy(struct config_blob *blob)
{
  if (blob) {
    free(blob->bytes);
    *blob = (struct config_blob){0};
  }
}

bool config_blob_set(struct config_blob *blob, const void *bytes, size_t size)
{
  if (!blob || (size && !bytes) || size > CONFIG_FILE_MAX_BYTES) {
    return false;
  }
  struct config_blob result = {.size = size};
  result.bytes = malloc(size + 1);
  if (!result.bytes) {
    return false;
  }
  if (size) {
    memcpy(result.bytes, bytes, size);
  }
  result.bytes[size] = '\0';
  result.digest.present = true;
  result.digest.size = size;
  struct install_digest digest = {0};
  bool ok = install_digest_begin(&digest) && install_digest_update(&digest, bytes, size) &&
      install_digest_finish(&digest, result.digest.sha256);
  install_digest_abort(&digest);
  if (!ok) {
    config_blob_destroy(&result);
    return false;
  }
  config_blob_destroy(blob);
  *blob = result;
  return true;
}

static bool blob_valid(const struct config_blob *blob)
{
  return blob && config_digest_valid(&blob->digest) &&
      (blob->digest.present ? blob->bytes && blob->size == blob->digest.size :
      !blob->bytes && !blob->size);
}

static bool digest_equal(const struct config_file_digest *left,
    const struct config_file_digest *right)
{
  return left->present == right->present && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

bool config_blob_equal(const struct config_blob *left, const struct config_blob *right)
{
  return blob_valid(left) && blob_valid(right) && digest_equal(&left->digest, &right->digest) &&
      (!left->digest.present || !memcmp(left->bytes, right->bytes, left->size));
}

static bool nonzero_guid(const uint8_t guid[16])
{
  uint8_t value = 0;
  for (unsigned i = 0; i < 16; ++i) {
    value |= guid[i];
  }
  return value != 0;
}

bool config_target_valid(const struct config_target_facts *facts)
{
  static const uint8_t esp_type[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
  };
  return facts && facts->valid && facts->domain && facts->partition &&
      facts->device_block_size >= 512 && facts->device_block_size <= 4096 &&
      !(facts->device_block_size & (facts->device_block_size - 1)) &&
      facts->block_count && facts->first_block < facts->device_block_count &&
      facts->block_count <= facts->device_block_count - facts->first_block &&
      nonzero_guid(facts->disk_guid) && nonzero_guid(facts->partition_guid) &&
      !memcmp(facts->partition_type, esp_type, sizeof(esp_type));
}

bool config_target_equal(const struct config_target_facts *left,
    const struct config_target_facts *right)
{
  return config_target_valid(left) && config_target_valid(right) &&
      left->domain == right->domain && left->device == right->device &&
      left->partition == right->partition && left->first_block == right->first_block &&
      left->block_count == right->block_count &&
      left->device_block_size == right->device_block_size &&
      left->device_block_count == right->device_block_count &&
      left->partition_attributes == right->partition_attributes &&
      !memcmp(left->disk_guid, right->disk_guid, 16) &&
      !memcmp(left->partition_guid, right->partition_guid, 16) &&
      !memcmp(left->partition_type, right->partition_type, 16);
}

static bool grant(handle_t handle, uint64_t protocol, uint64_t rights,
    struct config_storage_report *report)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return failed(report, status);
  }
  return (info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      (info.rights & rights) == rights) || failed(report, CALL_DENIED);
}

static handle_t parent_for(const struct config_storage_view *view, enum config_storage_file file)
{
  return file == CONFIG_LIMINE ? view->limine_parent : view->config_parent;
}

static bool read_name(handle_t parent, const char *name, size_t limit,
    struct config_blob *blob, struct config_storage_report *report)
{
  if (!blob || !name) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (!grant(parent, PROTOCOL_DIRECTORY, CONFIG_PARENT_READ, report)) {
    return false;
  }
  config_blob_destroy(blob);
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_lookup(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &file);
  if (status == CALL_NOT_FOUND) {
    return true;
  }
  if (status != CALL_OK) {
    return failed(report, status);
  }
  uint64_t size = 0;
  status = file_size(file, &size);
  bool ok = status == CALL_OK && size <= limit;
  unsigned char *bytes = ok ? malloc((size_t)size + 1) : NULL;
  if (ok && !bytes) {
    status = CALL_NO_MEMORY;
    ok = false;
  } else if (status == CALL_OK && !ok) {
    status = CALL_LIMIT;
  }
  for (size_t offset = 0; ok && offset < size;) {
    size_t count = 0, wanted = (size_t)size - offset;
    if (wanted > CONFIG_IO_BYTES) {
      wanted = CONFIG_IO_BYTES;
    }
    status = file_read(file, offset, bytes + offset, wanted, &count);
    ok = status == CALL_OK && count && count <= wanted;
    if (ok) {
      install_progress_advance(count, 0);
    }
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    offset += count;
  }
  uint64_t final_size = 0;
  if (ok) {
    status = file_size(file, &final_size);
    ok = status == CALL_OK && final_size == size;
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
  handle_close(file);
  if (ok) {
    ok = config_blob_set(blob, bytes, (size_t)size);
    if (!ok) {
      status = CALL_NO_MEMORY;
    }
  }
  free(bytes);
  return ok || failed(report, status);
}

bool config_storage_read(const struct config_storage_view *view, enum config_storage_file file,
    struct config_blob *blob, struct config_storage_report *report)
{
  if (!view || !config_storage_file_name(file)) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (report) {
    report->file = file;
  }
  install_progress_begin("Reading configuration file", INSTALL_PROGRESS_WORK, NULL);
  bool ok = read_name(parent_for(view, file), config_storage_file_name(file), CONFIG_FILE_MAX_BYTES,
      blob, report);
  install_progress_finish();
  return ok;
}

bool config_storage_read_record(const struct config_storage_view *view,
    struct config_blob *blob, struct config_storage_report *report)
{
  install_progress_begin("Reading configuration completion record", INSTALL_PROGRESS_WORK, NULL);
  bool ok = view ? read_name(view->config_parent, CONFIG_RECORD_NAME, CONFIG_RECORD_MAX_BYTES,
      blob, report) : failed(report, CALL_BAD_REQUEST);
  install_progress_finish();
  return ok;
}

bool config_storage_load(const struct config_storage_view *view,
    struct config_blob selected[CONFIG_SET_FILES], struct config_record *record,
    struct config_storage_report *report)
{
  if (!view || !selected || !record) {
    return failed(report, CALL_BAD_REQUEST);
  }
  struct config_blob completion = {0};
  struct config_record parsed = {0};
  bool ok = config_storage_read_record(view, &completion, report) &&
      completion.digest.present && config_record_parse(completion.bytes, completion.size,
      &parsed) && parsed.committed;
  if (!ok && report && report->status == CALL_OK) {
    failed(report, CALL_BAD_REQUEST);
  }
  for (unsigned i = 0; ok && i < CONFIG_SET_FILES; ++i) {
    ok = config_storage_read(view, i, &selected[i], report) &&
        digest_equal(&selected[i].digest, &parsed.selected[i]);
    if (!ok && report && report->status == CALL_OK) {
      failed(report, CALL_BAD_REQUEST);
    }
  }
  /* A concurrent publisher may replace the record during reads. Require the
   * same record again; frozen returned bytes never reopen paths during effects. */
  struct config_blob again = {0};
  if (ok) {
    ok = config_storage_read_record(view, &again, report) &&
        config_blob_equal(&completion, &again);
  }
  config_blob_destroy(&again);
  config_blob_destroy(&completion);
  if (ok) {
    *record = parsed;
  } else {
    if (report && report->status == CALL_OK) {
      failed(report, CALL_BAD_REQUEST);
    }
    *record = (struct config_record){0};
    for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) {
      config_blob_destroy(&selected[i]);
    }
  }
  return ok;
}

static bool parent_domain(handle_t parent, uint64_t domain, uint64_t rights,
    struct config_storage_report *report)
{
  if (!grant(parent, PROTOCOL_DIRECTORY, rights, report)) {
    return false;
  }
  struct file_info_reply info;
  enum call_status status = directory_info(parent, &info);
  if (status != CALL_OK) {
    return failed(report, status);
  }
  return ((info.valid & FILE_INFO_DOMAIN_VALID) && info.domain == domain) ||
      failed(report, CALL_DENIED);
}

static bool current_view(const struct config_storage_view *view,
    const struct config_target_facts *facts, bool limine, uint64_t interval,
    struct config_storage_report *report)
{
  if (!config_target_valid(facts) ||
      !grant(view->control, PROTOCOL_MOUNT, CONFIG_CONTROL_WRITE, report)) {
    return failed(report, CALL_DENIED);
  }
  struct mount_state_reply state;
  enum call_status status = mount_observe(view->control, &state);
  if (status != CALL_OK) {
    return failed(report, status);
  }
  if (state.domain != facts->domain || state.device != facts->device ||
      state.partition != facts->partition || state.first_block != facts->first_block ||
      state.block_count != facts->block_count ||
      !(state.flags & MOUNT_STATE_IDENTITY_VALID) ||
      state.device_block_size != facts->device_block_size ||
      state.device_block_count != facts->device_block_count ||
      state.partition_attributes != facts->partition_attributes ||
      memcmp(state.disk_guid, facts->disk_guid, sizeof(facts->disk_guid)) ||
      memcmp(state.partition_guid, facts->partition_guid, sizeof(facts->partition_guid)) ||
      memcmp(state.partition_type, facts->partition_type, sizeof(facts->partition_type)) ||
      !(state.flags & MOUNT_STATE_READ_WRITE) ||
      (state.flags & (MOUNT_STATE_RETIRED | MOUNT_STATE_FAILED | MOUNT_STATE_CLEANUP_PENDING))) {
    return failed(report, CALL_DENIED);
  }
  if (interval) {
    uint64_t now = 0;
    status = clock_now(view->clock, &now);
    if (status != CALL_OK) {
      return failed(report, status);
    }
    if (!(state.flags & MOUNT_STATE_INTERVAL) || state.interval_id != interval ||
        now >= state.interval_deadline_ns) {
      return failed(report, CALL_TIMED_OUT);
    }
  }
  return parent_domain(view->config_parent, state.domain, CONFIG_PARENT_WRITE, report) &&
      (view->limine_parent == HANDLE_INVALID ? !limine :
      parent_domain(view->limine_parent, state.domain,
          limine ? CONFIG_PARENT_WRITE : CONFIG_PARENT_READ, report));
}

bool config_storage_guard(const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval,
    struct config_storage_report *report)
{
  return current_view(view, facts, true, interval, report);
}

static bool expected_name(handle_t parent, const char *name,
    const struct config_blob *expected, struct config_storage_report *report)
{
  struct config_blob current = {0};
  bool ok = read_name(parent, name, CONFIG_FILE_MAX_BYTES, &current, report) &&
      config_blob_equal(&current, expected);
  config_blob_destroy(&current);
  return ok || failed(report, report && report->status != CALL_OK ?
      report->status : CALL_BAD_REQUEST);
}

static bool write_new(handle_t parent, const char *name, const struct config_blob *blob,
    struct config_storage_report *report)
{
  install_progress_begin("Staging configuration and recovery bytes", INSTALL_PROGRESS_WORK, NULL);
  install_progress_bytes_total(blob->size);
  if (report) {
    strcpy(report->artifact, name);
  }
  handle_t file = HANDLE_INVALID;
  enum call_status status = directory_create(parent, name, DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ | FILE_RIGHT_WRITE, &file);
  if (status != CALL_OK) {
    return failed(report, status);
  }
  bool ok = true;
  for (size_t offset = 0; ok && offset < blob->size;) {
    size_t count = 0, wanted = blob->size - offset;
    if (wanted > CONFIG_IO_BYTES) {
      wanted = CONFIG_IO_BYTES;
    }
    status = file_write(file, offset, blob->bytes + offset, wanted, &count);
    ok = status == CALL_OK && count && count <= wanted;
    if (ok) {
      install_progress_advance(count, 0);
    }
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    offset += count;
  }
  if (ok) {
    install_progress_begin("Syncing staged configuration file", INSTALL_PROGRESS_NONE, NULL);
    status = file_sync(file);
    ok = status == CALL_OK;
  }
  handle_close(file);
  install_progress_finish();
  return ok || failed(report, status);
}

static bool sync_mount(const struct config_storage_view *view,
    struct config_storage_report *report)
{
  install_progress_begin("Syncing configuration volume", INSTALL_PROGRESS_NONE, NULL);
  enum call_status status = mount_sync(view->control);
  install_progress_finish();
  return status == CALL_OK || failed(report, status);
}

static bool record_blob(const struct config_record *record, struct config_blob *blob)
{
  char *bytes = NULL;
  size_t size = 0;
  bool ok = config_record_render(record, &bytes, &size) && config_blob_set(blob, bytes, size);
  free(bytes);
  return ok;
}

static bool rename_file(handle_t parent, const char *from, const char *to,
    struct config_storage_report *report)
{
  strcpy(report->artifact, from);
  enum call_status status = directory_rename(parent, from, parent, to,
      DIRECTORY_RENAME_REPLACE, DIRECTORY_KIND_FILE);
  return status == CALL_OK || failed(report, status);
}

bool config_storage_check(const struct config_storage_view *view,
    const struct config_target_facts *facts,
    const struct config_blob expected[CONFIG_ALL_FILES],
    const struct config_blob *expected_record, bool limine_changed,
    struct config_storage_report *report)
{
  if (!view || !expected || !report || !blob_valid(expected_record)) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (!current_view(view, facts, limine_changed, 0, report)) {
    return false;
  }
  struct config_record previous = {0};
  if (expected_record->digest.present &&
      (!config_record_parse(expected_record->bytes, expected_record->size, &previous) ||
      !previous.committed)) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (!expected_name(view->config_parent, CONFIG_RECORD_NAME, expected_record, report)) {
    return false;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!blob_valid(&expected[i])) {
      return failed(report, CALL_BAD_REQUEST);
    }
    if (i == CONFIG_LIMINE && view->limine_parent == HANDLE_INVALID) {
      if (expected[i].digest.present || limine_changed) {
        return failed(report, CALL_BAD_REQUEST);
      }
      continue;
    }
    if (!expected_name(parent_for(view, i), config_storage_file_name(i), &expected[i], report)) {
      return false;
    }
    if (previous.obsolete[i].digest.present) {
      struct config_blob artifact = {0};
      bool ok = read_name(parent_for(view, i), previous.obsolete[i].name,
          CONFIG_FILE_MAX_BYTES, &artifact, report);
      bool unresolved = artifact.digest.present;
      config_blob_destroy(&artifact);
      if (!ok || unresolved) {
        return failed(report, report->status == CALL_OK ? CALL_BAD_REQUEST : report->status);
      }
    }
  }
  return true;
}

bool config_storage_publish_boot(const struct config_storage_view *view,
    const struct config_target_facts *facts,
    const struct config_blob expected[CONFIG_ALL_FILES],
    const struct config_blob *expected_record,
    const struct config_blob selected[CONFIG_ALL_FILES], const char *transaction,
    const char *revision, const struct install_staged_boot *boot,
    struct config_storage_report *report)
{
  if (!report) {
    return false;
  }
  *report = (struct config_storage_report){.phase = CONFIG_PHASE_CHECK};
  install_progress_begin("Checking configuration publication inputs", INSTALL_PROGRESS_WORK, NULL);
  if (!view || !expected || !selected || !blob_valid(expected_record) ||
      !config_transaction_valid(transaction) || !revision || strnlen(revision, 65) > 64) {
    return failed(report, CALL_BAD_REQUEST);
  }
  bool archive = boot && boot->binding.source == CONFIG_ACTIVE_ARCHIVE;
  bool changed[CONFIG_ALL_FILES] = {0};
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!blob_valid(&expected[i]) || !blob_valid(&selected[i]) ||
        (i < CONFIG_SET_FILES && selected[i].digest.present == archive)) {
      return failed(report, CALL_BAD_REQUEST);
    }
    changed[i] = !(archive && i < CONFIG_SET_FILES) &&
        !config_blob_equal(&expected[i], &selected[i]);
  }
  if (changed[CONFIG_LIMINE] && !selected[CONFIG_LIMINE].digest.present) {
    return failed(report, CALL_BAD_REQUEST);
  }
  uint64_t now = 0;
  enum call_status clock_status = clock_now(view->clock, &now);
  if (clock_status != CALL_OK) {
    return failed(report, clock_status);
  }
  if (!current_view(view, facts, changed[CONFIG_LIMINE], 0, report)) {
    return false;
  }
  /* Refuse an already stale plan before creating staging artifacts. Repeat
   * the same byte checks under the lease before any canonical-name swap. */
  if (!expected_name(view->config_parent, CONFIG_RECORD_NAME, expected_record, report)) {
    return false;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if ((i < CONFIG_SET_FILES || view->limine_parent != HANDLE_INVALID) &&
        !expected_name(parent_for(view, i), config_storage_file_name(i), &expected[i], report)) {
      return false;
    }
  }
  struct config_record previous = {0};
  if (expected_record->digest.present &&
      (!config_record_parse(expected_record->bytes, expected_record->size, &previous) ||
      !previous.committed)) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (expected_record->digest.present) {
    /* Owner edits need not match the previous selected hashes. The reviewed
     * current snapshot is rechecked before staging and under exclusive ownership;
     * prior hashes certify history, never authorize replacing current bytes. */
    /* Unresolved older recovery is retained for manual disposition. A new
     * publication must not silently discard names from a partial cleanup. */
    for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
      if (previous.obsolete[i].digest.present) {
        struct config_blob artifact = {0};
        bool ok = read_name(parent_for(view, i), previous.obsolete[i].name,
            CONFIG_FILE_MAX_BYTES, &artifact, report);
        bool unresolved = artifact.digest.present;
        config_blob_destroy(&artifact);
        if (!ok || unresolved) {
          return failed(report, report->status == CALL_OK ? CALL_BAD_REQUEST : report->status);
        }
      }
    }
  }
  struct config_record *record = &report->record;
  record->boot = boot ? boot->binding : previous.boot;
  record->previous_boot = boot ? previous.boot : previous.previous_boot;
  if (!boot && record->boot.present && record->boot.source == CONFIG_ACTIVE_ARCHIVE) {
    return failed(report, CALL_BAD_REQUEST);
  }
  if (record->boot.present && record->boot.source == CONFIG_ACTIVE_ESP &&
      !digest_equal(&record->boot.archive_catalog, &selected[CONFIG_CATALOG].digest)) {
    return failed(report, CALL_BAD_REQUEST);
  }
  strcpy(record->transaction, transaction);
  strcpy(record->revision, revision);
  for (unsigned i = 0; i < CONFIG_SET_FILES; ++i) {
    record->selected[i] = selected[i].digest;
  }
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (((!archive && i < CONFIG_SET_FILES) || changed[i]) && expected[i].digest.present) {
      record->previous[i].digest = expected[i].digest;
      config_record_artifact_name(transaction, i, "previous", record->previous[i].name);
    }
    record->obsolete[i] = previous.previous[i];
  }
  /* An unchanged Limine file carries its last recovery forward. A later Lua
   * publication must not delete the only recovery from an earlier Limine edit. */
  if (!record->previous[CONFIG_LIMINE].digest.present) {
    record->previous[CONFIG_LIMINE] = previous.previous[CONFIG_LIMINE];
    record->obsolete[CONFIG_LIMINE] = (struct config_recovery_file){0};
  }
  struct config_blob prepared = {0}, committed = {0};
  bool ok = record_blob(record, &prepared);
  record->committed = true;
  ok = ok && record_blob(record, &committed);
  record->committed = false;
  if (!ok) {
    config_blob_destroy(&prepared);
    config_blob_destroy(&committed);
    return failed(report, CALL_BAD_REQUEST);
  }
  char staged[CONFIG_ALL_FILES][CONFIG_ARTIFACT_NAME_BYTES] = {0};
  char prepared_name[CONFIG_ARTIFACT_NAME_BYTES], committed_name[CONFIG_ARTIFACT_NAME_BYTES];
  config_record_artifact_name(transaction, CONFIG_CATALOG, "prepared", prepared_name);
  config_record_artifact_name(transaction, CONFIG_CATALOG, "committed", committed_name);
  report->phase = CONFIG_PHASE_STAGE;
  install_progress_begin("Staging reviewed configuration publication", INSTALL_PROGRESS_WORK, NULL);
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    report->file = i;
    if (record->previous[i].digest.present && ((!archive && i < CONFIG_SET_FILES) || changed[i]) &&
        expected[i].digest.present) {
      ok = write_new(parent_for(view, i), record->previous[i].name, &expected[i], report);
    }
    if (ok && changed[i]) {
      config_record_artifact_name(transaction, i, "new", staged[i]);
      ok = write_new(parent_for(view, i), staged[i], &selected[i], report);
    }
  }
  if (ok) {
    ok = write_new(view->config_parent, prepared_name, &prepared, report) &&
        write_new(view->config_parent, committed_name, &committed, report) &&
        sync_mount(view, report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_VERIFY;
    install_progress_begin("Verifying staged configuration (filesystem view)",
        INSTALL_PROGRESS_WORK, NULL);
  }
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    if (record->previous[i].digest.present && ((!archive && i < CONFIG_SET_FILES) || changed[i]) &&
        expected[i].digest.present) {
      ok = expected_name(parent_for(view, i), record->previous[i].name, &expected[i], report);
    }
    if (ok && changed[i]) {
      ok = expected_name(parent_for(view, i), staged[i], &selected[i], report);
    }
  }
  if (ok) {
    ok = expected_name(view->config_parent, prepared_name, &prepared, report) &&
        expected_name(view->config_parent, committed_name, &committed, report);
  }
  struct mount_interval_reply interval = {0};
  if (ok) {
    report->phase = CONFIG_PHASE_EXCLUSIVE;
    install_progress_begin("Acquiring exclusive configuration interval", INSTALL_PROGRESS_NONE, NULL);
    enum call_status status = mount_begin(view->control, &interval);
    ok = status == CALL_OK || failed(report, status);
    if (ok) {
      install_progress_begin("Checking configuration volume under interval", INSTALL_PROGRESS_NONE, NULL);
      status = mount_check(view->control);
      ok = status == CALL_OK || failed(report, status);
    }
    if (ok) {
      install_progress_begin("Rechecking consumed and staged configuration under interval",
          INSTALL_PROGRESS_WORK, NULL);
    }
  }
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    if (i == CONFIG_LIMINE && view->limine_parent == HANDLE_INVALID) {
      continue;
    }
    ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report) &&
        expected_name(parent_for(view, i), config_storage_file_name(i), &expected[i], report);
  }
  if (ok) {
    ok = expected_name(view->config_parent, CONFIG_RECORD_NAME, expected_record, report) &&
        current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report);
  }
  /* Staging happened before admission. Verify its bytes again now that other
   * processes cannot mutate them, including the retained recovery copies. */
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report);
    if (ok && record->previous[i].digest.present &&
        ((!archive && i < CONFIG_SET_FILES) || changed[i]) && expected[i].digest.present) {
      ok = expected_name(parent_for(view, i), record->previous[i].name, &expected[i], report);
    }
    if (ok && changed[i]) {
      ok = expected_name(parent_for(view, i), staged[i], &selected[i], report);
    }
  }
  if (ok) {
    ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report) &&
        expected_name(view->config_parent, prepared_name, &prepared, report) &&
        expected_name(view->config_parent, committed_name, &committed, report);
  }
  if (ok && boot) {
    ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report) &&
        install_publication_verify(boot, facts->domain, report) &&
        current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_PREPARED;
    install_progress_begin("Publishing prepared configuration record", INSTALL_PROGRESS_NONE, NULL);
    ok = rename_file(view->config_parent, prepared_name, CONFIG_RECORD_NAME, report) &&
        sync_mount(view, report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_SWAP;
    install_progress_begin("Switching reviewed configuration names", INSTALL_PROGRESS_NONE, "files");
  }
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    if (changed[i] && (!boot || i != CONFIG_LIMINE)) {
      report->file = i;
      ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report) &&
          rename_file(parent_for(view, i), staged[i], config_storage_file_name(i), report);
      if (ok) {
        install_progress_advance(0, 1);
      }
    }
  }
  if (ok) {
    ok = sync_mount(view, report);
  }
  if (ok) {
    install_progress_begin("Verifying selected configuration (filesystem view)",
        INSTALL_PROGRESS_WORK, NULL);
  }
  for (unsigned i = 0; !archive && ok && i < CONFIG_SET_FILES; ++i) {
    ok = expected_name(view->config_parent, config_storage_file_name(i), &selected[i], report);
  }
  if (ok && changed[CONFIG_LIMINE] && !boot) {
    ok = expected_name(view->limine_parent, config_storage_file_name(CONFIG_LIMINE),
        &selected[CONFIG_LIMINE], report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_COMMITTED;
    install_progress_begin("Publishing committed configuration record", INSTALL_PROGRESS_NONE, NULL);
    ok = current_view(view, facts, changed[CONFIG_LIMINE], interval.id, report) &&
        rename_file(view->config_parent, committed_name, CONFIG_RECORD_NAME, report) &&
        sync_mount(view, report);
    if (ok) {
      install_progress_begin("Verifying committed configuration record", INSTALL_PROGRESS_WORK, NULL);
      ok = expected_name(view->config_parent, CONFIG_RECORD_NAME, &committed, report);
    }
    if (ok) {
      report->committed = true;
      record->committed = true;
    }
  }
  if (ok && boot) {
    report->phase = CONFIG_PHASE_ACTIVATE;
    report->boot_activation_pending = true;
    if (changed[CONFIG_LIMINE]) {
      report->file = CONFIG_LIMINE;
      ok = current_view(view, facts, true, interval.id, report) &&
          install_publication_verify(boot, facts->domain, report) &&
          current_view(view, facts, true, interval.id, report) &&
          expected_name(view->limine_parent, config_storage_file_name(CONFIG_LIMINE),
              &expected[CONFIG_LIMINE], report) &&
          expected_name(view->limine_parent, staged[CONFIG_LIMINE],
              &selected[CONFIG_LIMINE], report) &&
          current_view(view, facts, true, interval.id, report) &&
          rename_file(view->limine_parent, staged[CONFIG_LIMINE],
              config_storage_file_name(CONFIG_LIMINE), report) && sync_mount(view, report) &&
          expected_name(view->limine_parent, config_storage_file_name(CONFIG_LIMINE),
              &selected[CONFIG_LIMINE], report);
      if (ok) {
        report->boot_activated = true;
        report->boot_activation_pending = false;
        report->phase = CONFIG_PHASE_CLEANUP;
        install_progress_begin("Verifying recorded older boot payloads", INSTALL_PROGRESS_WORK, NULL);
        ok = current_view(view, facts, true, interval.id, report) &&
            install_publication_cleanup(boot, &previous.previous_boot, view, facts, interval.id, report);
      }
    }
  }
  if (ok) {
    report->phase = CONFIG_PHASE_CLEANUP;
    install_progress_begin("Verifying and removing recorded older recovery", INSTALL_PROGRESS_WORK,
        "files");
  }
  for (unsigned i = 0; ok && (!boot || report->boot_activated) && i < CONFIG_ALL_FILES; ++i) {
    const struct config_recovery_file *artifact = &record->obsolete[i];
    if (!artifact->digest.present) {
      continue;
    }
    struct config_blob bytes = {0};
    ok = current_view(view, facts, i == CONFIG_LIMINE, interval.id, report) &&
        read_name(parent_for(view, i), artifact->name, CONFIG_FILE_MAX_BYTES, &bytes, report) &&
        digest_equal(&bytes.digest, &artifact->digest);
    config_blob_destroy(&bytes);
    if (!ok) {
      if (report->status == CALL_OK) {
        failed(report, CALL_BAD_REQUEST);
      }
      break;
    }
    strcpy(report->artifact, artifact->name);
    enum call_status status = directory_remove(parent_for(view, i), artifact->name,
        DIRECTORY_KIND_FILE);
    ok = status == CALL_OK || failed(report, status);
    if (ok) {
      install_progress_advance(0, 1);
    }
  }
  if (ok) {
    ok = sync_mount(view, report);
  }
  if (interval.id) {
    install_progress_begin("Releasing configuration publication interval", INSTALL_PROGRESS_NONE, NULL);
    enum call_status status = mount_end(view->control, interval.id);
    if (ok && status != CALL_OK) {
      ok = failed(report, status);
    }
  }
  config_blob_destroy(&prepared);
  config_blob_destroy(&committed);
  if (ok) {
    report->phase = CONFIG_PHASE_DONE;
    report->artifact[0] = '\0';
  }
  install_progress_finish();
  return ok;
}

bool config_storage_publish(const struct config_storage_view *view,
    const struct config_target_facts *facts,
    const struct config_blob expected[CONFIG_ALL_FILES],
    const struct config_blob *expected_record,
    const struct config_blob selected[CONFIG_ALL_FILES], const char *transaction,
    const char *revision, struct config_storage_report *report)
{
  return config_storage_publish_boot(view, facts, expected, expected_record, selected,
      transaction, revision, NULL, report);
}

static bool pending_payload_open(const struct install_pending_request *request,
    const struct config_boot_binding *binding, struct install_esp_payloads **payloads,
    struct config_storage_report *report)
{
  struct install_esp_payload_request open = {.root = request->esp_root,
    .control = request->view.control, .workspace = request->workspace,
    .expected = request->expected_target, .revision = binding->kernel_revision,
    .attempt = binding->attempt};
  const struct config_boot_payload *files[] = {&binding->kernel, &binding->archive, &binding->revision};
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    open.expected_payloads[i].size = files[i]->digest.size;
    memcpy(open.expected_payloads[i].sha256, files[i]->digest.sha256, 65);
  }
  struct install_esp_payload_report readback;
  bool ok = install_esp_payload_open(&open, payloads, &readback);
  if (!ok) {
    snprintf(report->artifact, sizeof(report->artifact), "%s", readback.artifact);
    return failed(report, readback.status);
  }
  return true;
}

static bool pending_snapshot(const struct install_pending_request *request,
    const struct install_pending_plan *plan, uint64_t interval,
    struct config_storage_report *report)
{
  const struct config_storage_view *view = &request->view;
  bool ok = current_view(view, &request->facts, true, interval, report) &&
      expected_name(view->config_parent, CONFIG_RECORD_NAME, request->record, report);
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    ok = expected_name(parent_for(view, i), config_storage_file_name(i), &request->current[i], report);
  }
  if (ok && plan->menu_changed) {
    ok = expected_name(view->limine_parent, plan->record.previous[CONFIG_LIMINE].name,
        request->previous_limine, report);
  }
  return ok && current_view(view, &request->facts, true, interval, report);
}

bool config_storage_complete_pending(const struct install_pending_request *request,
    const struct install_pending_plan *plan, struct config_storage_report *report)
{
  if (!request || !plan || !plan->available || !report) {
    return failed(report, CALL_BAD_REQUEST);
  }
  const struct config_storage_view *view = &request->view;
  struct install_staged_boot boot = {.root = request->esp_root, .binding = plan->record.boot,
    .legacy_active = plan->legacy_active};
  report->record = plan->record;
  report->committed = true;
  report->boot_activation_pending = plan->menu_changed;
  struct install_esp_payloads *inherited = NULL;
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    boot.legacy_files[i] = HANDLE_INVALID;
    boot.legacy_expected[i] = request->legacy[i];
  }
  struct mount_interval_reply interval = {0};
  bool ok = pending_snapshot(request, plan, 0, report) &&
      pending_payload_open(request, &plan->record.boot, &boot.payloads, report);
  if (ok && !install_pending_selection(request, plan, boot.payloads)) {
    ok = failed(report, CALL_BAD_REQUEST);
  }
  if (ok && plan->menu_changed && plan->record.previous_boot.present) {
    ok = pending_payload_open(request, &plan->record.previous_boot, &inherited, report);
  }
  if (ok && plan->legacy_active) {
    ok = install_publication_legacy_hold(&boot, request->facts.domain, report);
  }
  char staged[CONFIG_ARTIFACT_NAME_BYTES];
  config_record_artifact_name(plan->record.transaction, CONFIG_LIMINE, "new", staged);
  if (ok && plan->menu_changed) {
    struct config_blob existing = {0};
    ok = read_name(view->limine_parent, staged, CONFIG_FILE_MAX_BYTES, &existing, report);
    if (ok && existing.digest.present) {
      ok = config_blob_equal(&existing, &plan->menu) || failed(report, CALL_BAD_REQUEST);
    } else if (ok) {
      /* Fresh explicit Review may create a known-absent staging name once.
       * An uncertain write never reaches this branch again in this operation. */
      ok = pending_snapshot(request, plan, 0, report) &&
          write_new(view->limine_parent, staged, &plan->menu, report) && sync_mount(view, report);
    }
    config_blob_destroy(&existing);
    if (ok) {
      ok = expected_name(view->limine_parent, staged, &plan->menu, report);
    }
  }
  if (ok) {
    report->phase = CONFIG_PHASE_EXCLUSIVE;
    enum call_status status = mount_begin(view->control, &interval);
    ok = status == CALL_OK || failed(report, status);
    if (ok) {
      status = mount_check(view->control);
      ok = status == CALL_OK || failed(report, status);
    }
  }
  for (unsigned pass = 0; ok && pass < 2; ++pass) {
    ok = pending_snapshot(request, plan, interval.id, report) &&
        install_publication_verify(&boot, request->facts.domain, report);
    if (ok && !install_pending_selection(request, plan, boot.payloads)) {
      ok = failed(report, CALL_BAD_REQUEST);
    }
    if (ok && inherited) {
      struct install_esp_payload_report readback;
      ok = install_esp_payload_verify(inherited, &readback);
      if (!ok) {
        failed(report, readback.status);
      }
    }
    if (ok) {
      ok = current_view(view, &request->facts, true, interval.id, report);
    }
  }
  if (ok) {
    report->phase = CONFIG_PHASE_ACTIVATE;
    report->committed = true;
    report->record = plan->record;
    report->boot_activation_pending = true;
    if (plan->menu_changed) {
      ok = expected_name(view->limine_parent, staged, &plan->menu, report) &&
          current_view(view, &request->facts, true, interval.id, report) &&
          rename_file(view->limine_parent, staged, config_storage_file_name(CONFIG_LIMINE), report) &&
          sync_mount(view, report);
    }
    if (ok) {
      ok = expected_name(view->limine_parent, config_storage_file_name(CONFIG_LIMINE),
          &plan->menu, report) && current_view(view, &request->facts, true, interval.id, report);
    }
    if (ok) {
      report->boot_activated = true;
      report->boot_activation_pending = false;
    }
  }
  if (ok) {
    report->phase = CONFIG_PHASE_CLEANUP;
  }
  for (unsigned i = 0; ok && i < CONFIG_ALL_FILES; ++i) {
    const struct config_recovery_file *old = &plan->record.obsolete[i];
    if (!old->digest.present) {
      continue;
    }
    struct config_blob bytes = {0};
    ok = current_view(view, &request->facts, true, interval.id, report) &&
        read_name(parent_for(view, i), old->name, CONFIG_FILE_MAX_BYTES, &bytes, report);
    if (ok && bytes.digest.present) {
      ok = digest_equal(&bytes.digest, &old->digest) || failed(report, CALL_BAD_REQUEST);
      if (ok) {
        strcpy(report->artifact, old->name);
        enum call_status status = directory_remove(parent_for(view, i), old->name, DIRECTORY_KIND_FILE);
        ok = status == CALL_OK || failed(report, status);
      }
    }
    config_blob_destroy(&bytes);
  }
  if (ok) {
    ok = sync_mount(view, report) && current_view(view, &request->facts, true, interval.id, report);
  }
  if (interval.id) {
    enum call_status status = mount_end(view->control, interval.id);
    if (ok && status != CALL_OK) {
      ok = failed(report, status);
    }
  }
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (boot.legacy_files[i] != HANDLE_INVALID) {
      handle_close(boot.legacy_files[i]);
    }
  }
  bool closed = install_esp_payload_close(inherited);
  closed = install_esp_payload_close(boot.payloads) && closed;
  if (ok && !closed) {
    ok = failed(report, CALL_BAD_HANDLE);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_DONE;
    report->artifact[0] = 0;
  }
  install_progress_finish();
  return ok;
}
