#include "publication.h"
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

#define BOOT_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define BOOT_IO_BYTES 3072u
#define BOOT_MANIFEST_BYTES 2048u
#define BOOT_SWEEP_ENTRIES 65536u

static const char *const payload_names[INSTALL_BOOT_PAYLOADS] = {
  "kernel.elf", "boot.arc", "revision.txt"
};

static bool fail(struct config_storage_report *report, enum call_status status)
{
  report->status = status;
  install_progress_finish();
  return false;
}

static const struct config_boot_payload *payload(const struct config_boot_binding *binding,
    unsigned index)
{
  return index == 0 ? &binding->kernel : index == 1 ? &binding->archive : &binding->revision;
}

static bool grant(handle_t handle, uint64_t protocol, uint64_t rights,
    struct config_storage_report *report)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return fail(report, status);
  }
  return (info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      (info.rights & rights) == rights) || fail(report, CALL_DENIED);
}

static bool directory_domain(handle_t directory, uint64_t domain,
    struct config_storage_report *report)
{
  struct file_info_reply info;
  enum call_status status = directory_info(directory, &info);
  return status == CALL_OK ? ((info.valid & FILE_INFO_DOMAIN_VALID) &&
      info.domain == domain) || fail(report, CALL_DENIED) : fail(report, status);
}

/* Hash/copy exactly the recorded extent. A short successful native transfer
 * advances only its known suffix; an error ends the operation without replay. */
static bool stream_source(const struct install_source *source, handle_t target,
    const struct config_file_digest *expected,
    struct config_storage_report *report)
{
  uint64_t size = 0;
  enum call_status status = CALL_OK;
  if (source->memory) {
    size = source->bytes;
  } else {
    status = file_size(source->handle, &size);
  }
  if (status != CALL_OK) {
    return fail(report, status);
  }
  if (!expected->present || size != expected->size || size > INSTALL_BOOT_PAYLOAD_MAX_BYTES) {
    return fail(report, CALL_BAD_REQUEST);
  }
  install_progress_begin(target == HANDLE_INVALID ? "Verifying boot payload bytes" :
      "Staging owned boot payload", INSTALL_PROGRESS_WORK, NULL);
  install_progress_bytes_total(size);
  struct install_digest hash = {0};
  bool ok = install_digest_begin(&hash);
  unsigned char bytes[BOOT_IO_BYTES];
  char sha256[65];
  for (uint64_t offset = 0; ok && offset < size;) {
    size_t count = 0;
    size_t wanted = size - offset > sizeof(bytes) ? sizeof(bytes) : (size_t)(size - offset);
    if (source->memory) {
      memcpy(bytes, source->memory + (size_t)offset, wanted);
      count = wanted;
      status = CALL_OK;
    } else {
      status = file_read(source->handle, offset, bytes, wanted, &count);
    }
    ok = status == CALL_OK && count && count <= wanted;
    if (status == CALL_OK && !ok) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (ok) {
      ok = install_digest_update(&hash, bytes, count);
      if (!ok) {
        status = CALL_NO_MEMORY;
      }
    }
    for (size_t written = 0; ok && target != HANDLE_INVALID && written < count;) {
      size_t amount = 0;
      status = file_write(target, offset + written, bytes + written, count - written, &amount);
      ok = status == CALL_OK && amount && amount <= count - written;
      if (status == CALL_OK && !ok) {
        status = CALL_OUTCOME_UNKNOWN;
      }
      written += amount;
    }
    if (ok) {
      offset += count;
      install_progress_advance(count, 0);
    }
  }
  if (ok) {
    if (!source->memory) {
      status = file_size(source->handle, &size);
    }
    ok = status == CALL_OK && size == expected->size;
    if (status == CALL_OK && !ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  if (ok) {
    ok = install_digest_finish(&hash, sha256) && !strcmp(sha256, expected->sha256);
    if (!ok) {
      status = CALL_BAD_REQUEST;
    }
  }
  install_digest_abort(&hash);
  install_progress_finish();
  return ok || fail(report, status == CALL_OK ? CALL_NO_MEMORY : status);
}

static bool payload_result(bool ok, const struct install_esp_payload_report *payload_report,
    struct config_storage_report *report)
{
  if (!ok) {
    snprintf(report->artifact, sizeof(report->artifact), "%s", payload_report->artifact);
    return fail(report, payload_report->status);
  }
  return true;
}

bool install_publication_verify(const struct install_staged_boot *boot,
    struct config_storage_report *report)
{
  struct install_esp_payload_report payload_report;
  bool ok = install_esp_payload_verify(boot->payloads, &payload_report);
  return payload_result(ok, &payload_report, report);
}

static bool folded_equal(const char *left, const char *right)
{
  for (;; ++left, ++right) {
    unsigned a = (unsigned char)*left, b = (unsigned char)*right;
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
    if (!a) return true;
  }
}

static bool manifest_line(const char **cursor, const char *end, const char *path,
    struct install_esp_payload_digest *digest)
{
  size_t path_size = strlen(path);
  const char *line = *cursor;
  if ((size_t)(end - line) < path_size + 1 ||
      memcmp(line, path, path_size) || line[path_size] != ' ') return false;
  line += path_size + 1;
  if (line == end || *line < '1' || *line > '9') return false;
  uint64_t size = 0;
  do {
    unsigned digit = *line++ - '0';
    if (size > (UINT64_MAX - digit) / 10) return false;
    size = size * 10 + digit;
  } while (line < end && *line >= '0' && *line <= '9');
  if (line == end || *line++ != ' ' || !size ||
      size > INSTALL_BOOT_PAYLOAD_MAX_BYTES || end - line < 65) return false;
  for (unsigned i = 0; i < 64; ++i) {
    if (!((*line >= '0' && *line <= '9') || (*line >= 'a' && *line <= 'f'))) return false;
    digest->sha256[i] = *line++;
  }
  if (*line++ != '\n') return false;
  digest->sha256[64] = '\0';
  digest->size = size;
  *cursor = line;
  return true;
}

static bool orphan_manifest(handle_t attempt, const char *revision, const char *id,
    struct install_esp_payload_digest digests[INSTALL_ESP_PAYLOAD_COUNT],
    unsigned *payload_count, struct config_storage_report *report)
{
  handle_t manifest = HANDLE_INVALID;
  enum call_status status = install_esp_child_open(attempt, INSTALL_BOOT_MANIFEST_NAME,
      DIRECTORY_KIND_FILE, FILE_RIGHT_READ, false, &manifest);
  if (status != CALL_OK) return fail(report, status);
  uint64_t size = 0;
  char bytes[BOOT_MANIFEST_BYTES + 1];
  size_t read = 0;
  status = file_size(manifest, &size);
  if (status == CALL_OK && (!size || size > BOOT_MANIFEST_BYTES)) status = CALL_BAD_REQUEST;
  if (status == CALL_OK) status = file_read(manifest, 0, bytes, (size_t)size, &read);
  if (status == CALL_OK && read != size) status = CALL_BAD_REQUEST;
  if (status == CALL_OK) {
    uint64_t again = 0;
    status = file_size(manifest, &again);
    if (status == CALL_OK && again != size) status = CALL_BAD_REQUEST;
  }
  if (handle_close(manifest) != CALL_OK && status == CALL_OK) status = CALL_BAD_HANDLE;
  if (status != CALL_OK) return fail(report, status);
  if (memchr(bytes, 0, (size_t)size)) return fail(report, CALL_BAD_REQUEST);
  bytes[size] = '\0';
  char header[256];
  int count = snprintf(header, sizeof(header),
      "PYXIS-BOOT-OWNERSHIP 1\nrevision %s\nattempt %s\n", revision, id);
  if (count <= 0 || count >= (int)sizeof(header) || (size_t)count > size ||
      strncmp(bytes, header, (size_t)count)) return fail(report, CALL_BAD_REQUEST);
  const char *cursor = bytes + count;
  const char *end = bytes + size;
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    char path[INSTALL_ESP_PAYLOAD_PATH_BYTES];
    if (!install_esp_payload_path(revision, id, i, path) ||
        !manifest_line(&cursor, end, path, &digests[i])) return fail(report, CALL_BAD_REQUEST);
  }
  *payload_count = INSTALL_BOOT_PAYLOADS;
  if (*cursor) {
    char path[INSTALL_ESP_PAYLOAD_PATH_BYTES];
    if (!install_esp_payload_path(revision, id, INSTALL_ESP_ENTRY, path) ||
        !manifest_line(&cursor, end, path, &digests[INSTALL_ESP_ENTRY])) {
      return fail(report, CALL_BAD_REQUEST);
    }
    *payload_count = INSTALL_ESP_PAYLOAD_COUNT;
  }
  return cursor == end || fail(report, CALL_BAD_REQUEST);
}

static bool orphan_attempt(const struct install_staged_boot *boot, handle_t revision_dir,
    const char *revision, const char *id, const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval, bool *removed,
    struct config_storage_report *report)
{
  *removed = false;
  snprintf(report->artifact, sizeof(report->artifact), "%s", id);
  handle_t attempt = HANDLE_INVALID;
  enum call_status status = install_esp_child_open(revision_dir, id,
      DIRECTORY_KIND_DIRECTORY, BOOT_DIRECTORY_RIGHTS, false, &attempt);
  if (status != CALL_OK) return fail(report, status);
  struct directory_cursor first_cursor = {0};
  char first_name[256];
  struct directory_enumerate_reply first;
  status = directory_enumerate(attempt, &first_cursor, first_name, sizeof(first_name), &first);
  if (status != CALL_OK || (first.outcome != DIRECTORY_END && first.outcome != DIRECTORY_ENTRY)) {
    handle_close(attempt);
    return fail(report, status == CALL_OK ? CALL_BAD_REQUEST : status);
  }
  /* A crash after removing the manifest but before removing its parent can
   * leave an empty directory. It holds no payload and proves no ownership;
   * leave the name alone rather than deleting a possibly foreign empty dir. */
  if (first.outcome == DIRECTORY_END) {
    status = handle_close(attempt);
    return status == CALL_OK || fail(report, status);
  }
  struct install_esp_payload_digest digests[INSTALL_ESP_PAYLOAD_COUNT] = {0};
  unsigned count = 0, seen = 0;
  bool ok = directory_domain(attempt, facts->domain, report) &&
      orphan_manifest(attempt, revision, id, digests, &count, report);
  struct directory_cursor cursor = {0};
  for (unsigned entries = 0; ok; ++entries) {
    if (entries == BOOT_SWEEP_ENTRIES) { ok = fail(report, CALL_LIMIT); break; }
    if (!(entries & 31u) && !config_storage_guard(view, facts, interval, report)) {
      ok = false;
      break;
    }
    char name[256];
    struct directory_enumerate_reply entry;
    status = directory_enumerate(attempt, &cursor, name, sizeof(name), &entry);
    if (status != CALL_OK) { ok = fail(report, status); break; }
    if (entry.outcome == DIRECTORY_END) break;
    if (entry.outcome != DIRECTORY_ENTRY || entry.kind != DIRECTORY_KIND_FILE) {
      ok = fail(report, CALL_BAD_REQUEST); break;
    }
    unsigned slot = 0;
    for (; slot < count; ++slot) {
      const char *expected = slot == INSTALL_ESP_ENTRY ? "entry.txt" : payload_names[slot];
      if (folded_equal(name, expected)) break;
    }
    if ((slot == count && !folded_equal(name, INSTALL_BOOT_MANIFEST_NAME)) ||
        (seen & (1u << slot))) { ok = fail(report, CALL_BAD_REQUEST); break; }
    seen |= 1u << slot;
    cursor = entry.cursor;
  }
  /* The manifest is written after staging and removed last. Missing payloads
   * are an interrupted earlier cleanup; present payloads still need hashes. */
  if (ok && !(seen & (1u << count))) ok = fail(report, CALL_BAD_REQUEST);
  for (unsigned i = 0; ok && i < count; ++i) {
    if (!(seen & (1u << i))) continue;
    const char *name = i == INSTALL_ESP_ENTRY ? "entry.txt" : payload_names[i];
    handle_t file = HANDLE_INVALID;
    status = install_esp_child_open(attempt, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, false, &file);
    ok = status == CALL_OK || fail(report, status);
    if (ok) {
      struct install_esp_payload_report readback;
      ok = install_esp_payload_verify_file(boot->payloads, file, &digests[i], &readback);
      if (!ok) fail(report, readback.status);
    }
    if (file != HANDLE_INVALID && handle_close(file) != CALL_OK) ok = fail(report, CALL_BAD_HANDLE);
  }
  for (unsigned i = 0; ok && i <= count; ++i) {
    if (!(seen & (1u << i))) continue;
    const char *name = i == count ? INSTALL_BOOT_MANIFEST_NAME :
        i == INSTALL_ESP_ENTRY ? "entry.txt" : payload_names[i];
    ok = config_storage_guard(view, facts, interval, report);
    if (ok) {
      status = directory_remove(attempt, name, DIRECTORY_KIND_FILE);
      ok = status == CALL_OK || fail(report, status);
    }
  }
  if (ok) {
    status = directory_sync(attempt);
    ok = status == CALL_OK || fail(report, status);
  }
  if (handle_close(attempt) != CALL_OK) ok = fail(report, CALL_BAD_HANDLE);
  if (ok) {
    ok = config_storage_guard(view, facts, interval, report);
    if (ok) {
      status = directory_remove(revision_dir, id, DIRECTORY_KIND_DIRECTORY);
      ok = status == CALL_OK || fail(report, status);
      *removed = ok;
    }
  }
  return ok;
}

bool install_publication_cleanup_orphans(const struct install_staged_boot *boot,
    const struct config_boot_binding *current, const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval,
    struct config_storage_report *report)
{
  handle_t efi = HANDLE_INVALID, pyxis = HANDLE_INVALID;
  enum call_status status = install_esp_child_open(boot->root, "EFI",
      DIRECTORY_KIND_DIRECTORY, BOOT_DIRECTORY_RIGHTS, false, &efi);
  if (status != CALL_OK) return fail(report, status);
  status = install_esp_child_open(efi, "pyxis", DIRECTORY_KIND_DIRECTORY,
      BOOT_DIRECTORY_RIGHTS, false, &pyxis);
  bool ok = status == CALL_OK || fail(report, status);
  struct directory_cursor cursor = {0};
  for (unsigned entries = 0; ok; ++entries) {
    if (entries == BOOT_SWEEP_ENTRIES) { ok = fail(report, CALL_LIMIT); break; }
    if (!(entries & 31u) && !config_storage_guard(view, facts, interval, report)) {
      ok = false;
      break;
    }
    char revision[256];
    struct directory_enumerate_reply entry;
    status = directory_enumerate(pyxis, &cursor, revision, sizeof(revision), &entry);
    if (status != CALL_OK) { ok = fail(report, status); break; }
    if (entry.outcome == DIRECTORY_END) break;
    if (entry.outcome != DIRECTORY_ENTRY || entry.kind != DIRECTORY_KIND_DIRECTORY) {
      ok = fail(report, CALL_BAD_REQUEST); break;
    }
    cursor = entry.cursor;
    if (folded_equal(revision, "config")) continue;
    snprintf(report->artifact, sizeof(report->artifact), "%s", revision);
    if (!config_revision_valid(revision)) { ok = fail(report, CALL_BAD_REQUEST); break; }
    handle_t revision_dir = HANDLE_INVALID;
    status = install_esp_child_open(pyxis, revision, DIRECTORY_KIND_DIRECTORY,
        BOOT_DIRECTORY_RIGHTS, false, &revision_dir);
    ok = status == CALL_OK || fail(report, status);
    if (ok) ok = directory_domain(revision_dir, facts->domain, report);
    struct directory_cursor attempts = {0};
    for (unsigned found = 0; ok; ++found) {
      if (found == BOOT_SWEEP_ENTRIES) { ok = fail(report, CALL_LIMIT); break; }
      if (!(found & 31u) && !config_storage_guard(view, facts, interval, report)) {
        ok = false;
        break;
      }
      char id[256];
      struct directory_enumerate_reply item;
      status = directory_enumerate(revision_dir, &attempts, id, sizeof(id), &item);
      if (status != CALL_OK) { ok = fail(report, status); break; }
      if (item.outcome == DIRECTORY_END) break;
      snprintf(report->artifact, sizeof(report->artifact), "%s", id);
      if (item.outcome == DIRECTORY_ENTRY && item.kind == DIRECTORY_KIND_FILE &&
          folded_equal(revision, "recovery") &&
          (folded_equal(id, "kernel.elf") || folded_equal(id, "boot.arc") ||
           folded_equal(id, "ownership.record"))) {
        attempts = item.cursor;
        continue;
      }
      if (item.outcome != DIRECTORY_ENTRY || item.kind != DIRECTORY_KIND_DIRECTORY ||
          !config_transaction_valid(id)) { ok = fail(report, CALL_BAD_REQUEST); break; }
      if (folded_equal(revision, current->kernel_revision) && !strcmp(id, current->attempt)) {
        attempts = item.cursor;
        continue;
      }
      bool removed = false;
      ok = orphan_attempt(boot, revision_dir, revision, id, view, facts,
          interval, &removed, report);
      attempts = removed ? (struct directory_cursor){0} : item.cursor;
    }
    if (revision_dir != HANDLE_INVALID && handle_close(revision_dir) != CALL_OK) {
      ok = fail(report, CALL_BAD_HANDLE);
    }
  }
  if (pyxis != HANDLE_INVALID && handle_close(pyxis) != CALL_OK) ok = fail(report, CALL_BAD_HANDLE);
  if (handle_close(efi) != CALL_OK) ok = fail(report, CALL_BAD_HANDLE);
  return ok;
}

bool install_publication_publish(const struct install_publication_request *request,
    struct config_storage_report *report)
{
  if (!report) {
    return false;
  }
  *report = (struct config_storage_report){.phase = CONFIG_PHASE_CHECK};
  if (!request || !request->expected || !request->selected ||
      !request->binding.present || !config_boot_binding_valid(&request->binding) ||
      !config_transaction_valid(request->transaction) ||
      strcmp(request->transaction, request->binding.attempt) ||
      !config_blob_equal(request->selected, request->selected)) {
    return fail(report, CALL_BAD_REQUEST);
  }
  bool menu_changed = !config_blob_equal(request->expected, request->selected);
  if (!grant(request->esp_root, PROTOCOL_DIRECTORY, BOOT_DIRECTORY_RIGHTS, report) ||
      !directory_domain(request->esp_root, request->facts.domain, report) ||
      !config_storage_check(&request->view, &request->facts, request->expected,
          request->expected_record, menu_changed, report)) {
    return false;
  }
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    const struct config_file_digest *digest = &payload(&request->binding, i)->digest;
    if (request->sources[i].bytes != digest->size || digest->size > INSTALL_BOOT_PAYLOAD_MAX_BYTES ||
        (!request->sources[i].memory && !grant(request->sources[i].handle, PROTOCOL_FILE,
        FILE_RIGHT_READ, report)) || !stream_source(&request->sources[i], HANDLE_INVALID, digest, report)) {
      return fail(report, report->status == CALL_OK ? CALL_LIMIT : report->status);
    }
  }
  /* Source hashing may take time. Repeat all consumed destination byte checks
   * immediately before the first namespace mutation. */
  if (!config_storage_check(&request->view, &request->facts, request->expected,
      request->expected_record, menu_changed, report)) {
    return false;
  }
  struct install_staged_boot staged = {.root = request->esp_root, .binding = request->binding};
  bool ok = true;
  report->phase = CONFIG_PHASE_STAGE;
  struct install_esp_payload_request payload_request = {
    .root = request->esp_root, .control = request->view.control,
    .workspace = request->workspace,
    .expected = request->expected_target,
    .revision = request->binding.kernel_revision, .attempt = request->binding.attempt,
  };
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    payload_request.sources[i] = request->sources[i];
    const struct config_file_digest *digest = &payload(&request->binding, i)->digest;
    payload_request.expected_payloads[i].size = digest->size;
    strcpy(payload_request.expected_payloads[i].sha256, digest->sha256);
  }
  if (ok) {
    struct install_esp_payload_report payload_report;
    bool staged_ok = install_esp_payload_stage(&payload_request, &staged.payloads, &payload_report);
    ok = payload_result(staged_ok, &payload_report, report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_VERIFY;
    ok = config_storage_publish_boot(&request->view, &request->facts, request->expected,
        request->expected_record, request->selected, request->transaction, &staged, report);
  }
  bool payloads_closed = install_esp_payload_close(staged.payloads);
  staged.payloads = NULL;
  if (!payloads_closed) {
    if (report->status == CALL_OK) {
      report->phase = CONFIG_PHASE_CLEANUP;
      fail(report, CALL_BAD_HANDLE);
    }
    ok = false;
  }
  install_progress_finish();
  return ok;
}
