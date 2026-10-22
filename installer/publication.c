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
#include <string.h>

#define BOOT_DIRECTORY_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | DIRECTORY_RIGHT_READ_FILES | \
    DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_REMOVE)
#define BOOT_IO_BYTES 3072u
#define BOOT_DIRECTORY_READ (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES)

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
    struct config_storage_report *report)
{
  struct install_esp_payload_report payload_report;
  bool ok = install_esp_payload_verify(boot->payloads, &payload_report);
  return payload_result(ok, &payload_report, report);
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
