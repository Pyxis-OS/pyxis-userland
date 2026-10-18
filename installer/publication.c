#include "publication.h"
#include "esp_directory.h"
#include "digest.h"
#include "progress.h"

#include <directory.h>
#include <file.h>
#include <handle.h>
#include <mount.h>
#include <stdio.h>
#include <string.h>

#define BOOT_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define BOOT_IO_BYTES 3072u
#define BOOT_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)

static const char *const payload_names[INSTALL_BOOT_PAYLOADS] = {
  "kernel.elf", "boot.arc", "revision.txt"
};

static const char *const legacy_names[INSTALL_BOOT_PAYLOADS] = {
  "caelum.elf", "initrd.cpio", "revision"
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

static bool same_object(handle_t original, handle_t reopened, bool directory,
    uint64_t domain, struct config_storage_report *report)
{
  struct file_info_reply held, current;
  enum call_status status = directory ? directory_info(original, &held) : file_info(original, &held);
  if (status == CALL_OK) {
    status = directory ? directory_info(reopened, &current) : file_info(reopened, &current);
  }
  if (status != CALL_OK) {
    return fail(report, status);
  }
  return ((held.valid & FILE_INFO_OBJECT_VALID) && (current.valid & FILE_INFO_OBJECT_VALID) &&
      held.domain == domain && current.domain == domain && held.object == current.object) ||
      fail(report, CALL_BAD_REQUEST);
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

static bool stream(handle_t source, handle_t target,
    const struct config_file_digest *expected, struct config_storage_report *report)
{
  struct install_source held = {.handle = source, .bytes = expected->size};
  return stream_source(&held, target, expected, report);
}

static bool legacy_open(handle_t root,
    const struct config_file_digest expected[INSTALL_BOOT_PAYLOADS],
    uint64_t domain, handle_t files[INSTALL_BOOT_PAYLOADS],
    struct config_storage_report *report)
{
  handle_t parent = HANDLE_INVALID;
  strcpy(report->artifact, "boot");
  enum call_status status = directory_lookup(root, "boot", DIRECTORY_KIND_DIRECTORY,
      BOOT_DIRECTORY_READ, &parent);
  bool ok = status == CALL_OK || fail(report, status);
  if (ok) {
    ok = grant(parent, PROTOCOL_DIRECTORY, BOOT_DIRECTORY_READ, report) &&
        directory_domain(parent, domain, report);
  }
  for (unsigned i = 0; ok && i < INSTALL_BOOT_PAYLOADS; ++i) {
    snprintf(report->artifact, sizeof(report->artifact), "boot/%s", legacy_names[i]);
    if (!config_payload_digest_valid(&expected[i]) || !expected[i].present ||
        !expected[i].size || expected[i].size > INSTALL_BOOT_PAYLOAD_MAX_BYTES) {
      ok = fail(report, CALL_BAD_REQUEST);
      break;
    }
    status = directory_lookup(parent, legacy_names[i], DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &files[i]);
    ok = status == CALL_OK || fail(report, status);
    if (ok) {
      struct file_info_reply info;
      status = file_info(files[i], &info);
      ok = status == CALL_OK || fail(report, status);
      if (ok) {
        ok = grant(files[i], PROTOCOL_FILE, FILE_RIGHT_READ, report) &&
            (((info.valid & FILE_INFO_OBJECT_VALID) && info.domain == domain) ||
            fail(report, CALL_DENIED)) &&
            stream(files[i], HANDLE_INVALID, &expected[i], report);
      }
    }
  }
  if (parent != HANDLE_INVALID) {
    handle_close(parent);
  }
  return ok;
}

bool install_publication_legacy_check(handle_t root,
    const struct config_file_digest expected[INSTALL_BOOT_PAYLOADS],
    struct config_storage_report *report)
{
  if (!expected || !report) {
    return false;
  }
  if (!grant(root, PROTOCOL_DIRECTORY, BOOT_DIRECTORY_READ, report)) {
    return false;
  }
  struct file_info_reply info;
  enum call_status status = directory_info(root, &info);
  if (status != CALL_OK || !(info.valid & FILE_INFO_DOMAIN_VALID)) {
    return fail(report, status == CALL_OK ? CALL_DENIED : status);
  }
  handle_t files[INSTALL_BOOT_PAYLOADS];
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    files[i] = HANDLE_INVALID;
  }
  bool ok = legacy_open(root, expected, info.domain, files, report);
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (files[i] != HANDLE_INVALID) {
      handle_close(files[i]);
    }
  }
  return ok;
}

static bool legacy_verify(const struct install_staged_boot *boot, uint64_t domain,
    struct config_storage_report *report)
{
  if (!boot->legacy_active) {
    return true;
  }
  handle_t parent = HANDLE_INVALID;
  strcpy(report->artifact, "boot");
  enum call_status status = directory_lookup(boot->root, "boot", DIRECTORY_KIND_DIRECTORY,
      BOOT_DIRECTORY_READ, &parent);
  bool ok = status == CALL_OK || fail(report, status);
  for (unsigned i = 0; ok && i < INSTALL_BOOT_PAYLOADS; ++i) {
    snprintf(report->artifact, sizeof(report->artifact), "boot/%s", legacy_names[i]);
    handle_t file = HANDLE_INVALID;
    status = directory_lookup(parent, legacy_names[i], DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ, &file);
    ok = status == CALL_OK || fail(report, status);
    if (ok) {
      const struct config_file_digest *expected = &boot->legacy_expected[i];
      struct install_esp_payload_digest digest = {.size = expected->size};
      memcpy(digest.sha256, expected->sha256, sizeof(digest.sha256));
      struct install_esp_payload_report readback;
      ok = same_object(boot->legacy_files[i], file, false, domain, report);
      if (ok && !install_esp_payload_verify_file(boot->payloads, file, &digest, &readback)) {
        ok = fail(report, readback.status);
      }
    }
    if (file != HANDLE_INVALID) {
      handle_close(file);
    }
  }
  if (parent != HANDLE_INVALID) {
    handle_close(parent);
  }
  return ok;
}

static bool manifest_blob(const struct config_boot_binding *binding, struct config_blob *blob)
{
  char bytes[2048];
  int count = snprintf(bytes, sizeof(bytes), "PYXIS-BOOT-OWNERSHIP 1\nrevision %s\nattempt %s\n",
      binding->kernel_revision, binding->attempt);
  size_t used = count > 0 ? (size_t)count : sizeof(bytes);
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS && used < sizeof(bytes); ++i) {
    const struct config_boot_payload *item = payload(binding, i);
    count = snprintf(bytes + used, sizeof(bytes) - used, "%s %llu %s\n", item->path,
        (unsigned long long)item->digest.size, item->digest.sha256);
    used += count > 0 ? (size_t)count : sizeof(bytes);
  }
  return used < sizeof(bytes) && config_blob_set(blob, bytes, used);
}

/* Cleanup traverses only canonical components. FAT short aliases confer no
 * ownership, including for revision/attempt directories inside EFI/pyxis. */
static bool open_attempt(handle_t root, const struct config_boot_binding *binding,
    handle_t *attempt, struct config_storage_report *report)
{
  const char *components[] = {"EFI", "pyxis", binding->kernel_revision, binding->attempt};
  handle_t parent = root;
  bool owned = false;
  for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); ++i) {
    handle_t child = HANDLE_INVALID;
    enum call_status status = install_esp_child_open(parent, components[i],
        DIRECTORY_KIND_DIRECTORY, BOOT_DIRECTORY_RIGHTS, false, &child);
    if (owned && handle_close(parent) != CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status != CALL_OK) {
      if (child != HANDLE_INVALID) {
        handle_close(child);
      }
      return fail(report, status);
    }
    parent = child;
    owned = true;
  }
  *attempt = parent;
  return true;
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
    uint64_t domain, struct config_storage_report *report)
{
  struct install_esp_payload_report payload_report;
  bool ok = install_esp_payload_verify(boot->payloads, &payload_report);
  return payload_result(ok, &payload_report, report) && legacy_verify(boot, domain, report);
}

bool install_publication_cleanup(const struct install_staged_boot *boot,
    const struct config_boot_binding *obsolete, const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval,
    struct config_storage_report *report)
{
  if (!obsolete->present) {
    return true;
  }
  /* Hash evidence certifies only the exact three files. Uncertain evidence
   * retains all names; no prefix sweep or automatic repair follows. */
  struct config_storage_report inspect = {0};
  struct config_blob manifest = {0};
  handle_t parent = HANDLE_INVALID, files[INSTALL_BOOT_PAYLOADS + 1];
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS + 1; ++i) {
    files[i] = HANDLE_INVALID;
  }
  bool ok = config_boot_binding_valid(obsolete) &&
      (manifest_blob(obsolete, &manifest) || fail(&inspect, CALL_NO_MEMORY)) &&
      open_attempt(boot->root, obsolete, &parent, &inspect) &&
      directory_domain(parent, facts->domain, &inspect);
  for (unsigned i = 0; ok && i < INSTALL_BOOT_PAYLOADS + 1; ++i) {
    const char *name = i == INSTALL_BOOT_PAYLOADS ? INSTALL_BOOT_MANIFEST_NAME : payload_names[i];
    enum call_status status = install_esp_child_open(parent, name,
        DIRECTORY_KIND_FILE, FILE_RIGHT_READ, false, &files[i]);
    if (status != CALL_OK) {
      inspect.status = status;
    }
    if (status == CALL_OK) {
      const struct config_file_digest *expected = i == INSTALL_BOOT_PAYLOADS ?
          &manifest.digest : &payload(obsolete, i)->digest;
      struct install_esp_payload_digest digest = {.size = expected->size};
      memcpy(digest.sha256, expected->sha256, sizeof(digest.sha256));
      struct install_esp_payload_report readback;
      ok = install_esp_payload_verify_file(boot->payloads, files[i], &digest, &readback);
      inspect.status = readback.status;
    } else {
      ok = false;
    }
  }
  if (!ok) {
    if (inspect.status != CALL_OK && inspect.status != CALL_NOT_FOUND &&
        inspect.status != CALL_BAD_REQUEST && inspect.status != CALL_DENIED) {
      ok = fail(report, inspect.status);
    } else {
      report->boot_cleanup_skipped = true;
      ok = true;
    }
  } else {
    for (unsigned i = 0; ok && i < INSTALL_BOOT_PAYLOADS + 1; ++i) {
      const char *name = i == INSTALL_BOOT_PAYLOADS ? INSTALL_BOOT_MANIFEST_NAME : payload_names[i];
      if (!config_storage_guard(view, facts, interval, report)) {
        ok = false;
        break;
      }
      enum call_status status = directory_remove(parent, name, DIRECTORY_KIND_FILE);
      ok = status == CALL_OK || fail(report, status);
    }
    if (ok) {
      enum call_status status = directory_sync(parent);
      ok = status == CALL_OK || fail(report, status);
    }
  }
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS + 1; ++i) {
    if (files[i] != HANDLE_INVALID) {
      handle_close(files[i]);
    }
  }
  if (parent != HANDLE_INVALID) {
    handle_close(parent);
  }
  config_blob_destroy(&manifest);
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
      !config_revision_valid(request->revision)) {
    return fail(report, CALL_BAD_REQUEST);
  }
  bool archive = request->binding.source == CONFIG_ACTIVE_ARCHIVE;
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!config_blob_equal(&request->selected[i], &request->selected[i]) ||
        (i < CONFIG_SET_FILES && request->selected[i].digest.present == archive)) {
      return fail(report, CALL_BAD_REQUEST);
    }
  }
  if (!archive && (request->selected[CONFIG_CATALOG].digest.size !=
      request->binding.archive_catalog.size || strcmp(
      request->selected[CONFIG_CATALOG].digest.sha256, request->binding.archive_catalog.sha256))) {
    return fail(report, CALL_BAD_REQUEST);
  }
  if (!grant(request->esp_root, PROTOCOL_DIRECTORY, BOOT_DIRECTORY_RIGHTS, report) ||
      !directory_domain(request->esp_root, request->facts.domain, report) ||
      !config_storage_check(&request->view, &request->facts, request->expected,
          request->expected_record, !config_blob_equal(&request->expected[CONFIG_LIMINE],
          &request->selected[CONFIG_LIMINE]), report)) {
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
      request->expected_record, !config_blob_equal(&request->expected[CONFIG_LIMINE],
      &request->selected[CONFIG_LIMINE]), report)) {
    return false;
  }
  struct install_staged_boot staged = {.root = request->esp_root, .binding = request->binding,
      .legacy_active = request->legacy_active};
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    staged.legacy_files[i] = HANDLE_INVALID;
    if (staged.legacy_active) {
      staged.legacy_expected[i] = request->legacy_expected[i];
    }
  }
  bool ok = !staged.legacy_active || legacy_open(staged.root, staged.legacy_expected,
      request->facts.domain, staged.legacy_files, report);
  if (ok) {
    ok = config_storage_check(&request->view, &request->facts, request->expected,
        request->expected_record, !config_blob_equal(&request->expected[CONFIG_LIMINE],
        &request->selected[CONFIG_LIMINE]), report);
  }
  if (ok) {
    report->phase = CONFIG_PHASE_STAGE;
  }
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
        request->expected_record, request->selected, request->transaction, request->revision,
        &staged, report);
  }
  for (unsigned i = 0; i < INSTALL_BOOT_PAYLOADS; ++i) {
    if (staged.legacy_files[i] != HANDLE_INVALID) {
      handle_close(staged.legacy_files[i]);
    }
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
