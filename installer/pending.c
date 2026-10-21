#include "pending.h"
#include "boot_entry.h"

#include <string.h>

static bool digest_equal(const struct config_file_digest *left,
    const struct config_file_digest *right)
{
  return left->present == right->present && left->size == right->size &&
      !strcmp(left->sha256, right->sha256);
}

void install_pending_destroy(struct install_pending_plan *plan)
{
  if (plan) {
    config_blob_destroy(&plan->menu);
    *plan = (struct install_pending_plan){0};
  }
}

bool install_pending_inspect(const struct config_blob *record,
    const struct config_blob current[CONFIG_ALL_FILES],
    const struct config_blob *previous_limine, const uint8_t disk_guid[16],
    const struct config_file_digest legacy[INSTALL_BOOT_PAYLOADS],
    struct install_pending_plan *plan)
{
  if (!record || !current || !previous_limine || !disk_guid || !plan) {
    return false;
  }
  install_pending_destroy(plan);
  if (!record->digest.present) {
    return true;
  }
  plan->state = INSTALL_PENDING_REFUSED;
  if (!config_blob_equal(record, record) ||
      !config_record_parse(record->bytes, record->size, &plan->record) ||
      !plan->record.committed) {
    return true;
  }
  if (!plan->record.boot.present) {
    plan->state = INSTALL_PENDING_NONE;
    return true;
  }
  const struct config_record *history = &plan->record;
  if (history->boot.source == CONFIG_ACTIVE_ESP &&
      !digest_equal(&history->boot.archive_catalog, &history->selected[CONFIG_CATALOG])) {
    return true;
  }
  bool selection_matches = true;
  for (unsigned i = 0; i < CONFIG_ALL_FILES; ++i) {
    if (!config_blob_equal(&current[i], &current[i])) return true;
    if (i < CONFIG_SET_FILES && history->boot.source == CONFIG_ACTIVE_ESP &&
        !digest_equal(&history->selected[i], &current[i].digest)) selection_matches = false;
  }
  const struct install_boot_entry_paths active = {
    history->boot.kernel.path, history->boot.archive.path};
  const struct install_boot_entry_paths inherited = {
    history->previous_boot.kernel.path, history->previous_boot.archive.path};
  const struct install_boot_menu committed = {&active,
    history->previous_boot.present ? &inherited : NULL, false};
  bool adoption = false, pending = true, is_current = false;
  if (!install_boot_entry_current(&current[CONFIG_LIMINE], &committed, disk_guid, &is_current)) {
    return false;
  }
  if (is_current) {
    if (!config_blob_set(&plan->menu, current[CONFIG_LIMINE].bytes, current[CONFIG_LIMINE].size)) {
      return false;
    }
    plan->state = INSTALL_PENDING_MENU_CURRENT;
    /* A completed menu permits an ordinary new Review of owner edits. Those
     * edits are not the committed set and cannot authorize pending cleanup. */
    plan->available = selection_matches;
    return true;
  }
  if (!selection_matches) return true;
  const struct config_recovery_file *old = &history->previous[CONFIG_LIMINE];
  if (!old->digest.present || !config_blob_equal(previous_limine, &current[CONFIG_LIMINE]) ||
      !digest_equal(&old->digest, &previous_limine->digest)) {
    return true;
  }
  bool legacy_valid = !history->previous_boot.present && legacy;
  for (unsigned i = 0; legacy_valid && i < INSTALL_BOOT_PAYLOADS; ++i) {
    legacy_valid = config_payload_digest_valid(&legacy[i]) && legacy[i].present && legacy[i].size;
  }
  if (!history->previous_boot.present && !legacy_valid) {
    return true;
  }
  /* The digest proof above owns its previous entry, whatever attempt it names. */
  const struct install_boot_menu replaced = {&inherited, NULL, true};
  if (!install_boot_entry_plan(&current[CONFIG_LIMINE],
      history->previous_boot.present ? &replaced : NULL, disk_guid, &committed,
      legacy_valid, &plan->menu, &adoption, &pending)) {
    return false;
  }
  plan->available = !pending;
  if (!pending) {
    plan->state = INSTALL_PENDING_ACTIVATION;
  }
  plan->menu_changed = !pending && !config_blob_equal(&plan->menu, &current[CONFIG_LIMINE]);
  plan->legacy_active = !pending && adoption;
  return true;
}

bool install_pending_complete(const struct install_pending_request *request,
    struct config_storage_report *report)
{
  if (!report) {
    return false;
  }
  *report = (struct config_storage_report){.phase = CONFIG_PHASE_CHECK};
  if (!request || !request->fallback_ready) {
    report->status = CALL_DENIED;
    return false;
  }
  struct install_pending_plan plan = {0};
  bool ok = install_pending_inspect(request->record, request->current,
      request->previous_limine, request->facts.disk_guid, request->legacy, &plan) && plan.available;
  if (!ok) {
    report->status = CALL_BAD_REQUEST;
  } else {
    ok = config_storage_complete_pending(request, &plan, report);
  }
  install_pending_destroy(&plan);
  return ok;
}
