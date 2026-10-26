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
    const struct config_blob *current_menu,
    const struct config_blob *previous_limine, const uint8_t disk_guid[16],
    uint64_t pool_partition, struct install_pending_plan *plan)
{
  if (!record || !current_menu || !previous_limine || !disk_guid || !plan) {
    return false;
  }
  install_pending_destroy(plan);
  if (!record->digest.present) {
    return true;
  }
  plan->state = INSTALL_PENDING_REFUSED;
  if (!config_blob_equal(record, record) ||
      !config_record_parse(record->bytes, record->size, &plan->record) ||
      !plan->record.committed || !config_blob_equal(current_menu, current_menu)) {
    return true;
  }
  const struct config_record *history = &plan->record;
  const struct install_boot_entry_paths active = {
    history->boot.kernel.path, history->boot.archive.path};
  const struct install_boot_entry_paths legacy = {
    history->legacy_previous_boot.kernel.path, history->legacy_previous_boot.archive.path};
  const struct install_boot_menu committed = {&active,
    history->legacy_previous_boot.present ? &legacy : NULL, false};
  bool pending = true, is_current = false;
  if (!install_boot_entry_current(current_menu, &committed, disk_guid, pool_partition,
      &is_current)) {
    return false;
  }
  if (is_current) {
    if (!config_blob_set(&plan->menu, current_menu->bytes, current_menu->size)) {
      return false;
    }
    plan->state = INSTALL_PENDING_MENU_CURRENT;
    plan->available = true;
    return true;
  }
  /* An interrupted 0.0.9 publication would select its previous entry.
   * Leave its old-format record for explicit recovery instead of publishing
   * a menu with a retired boot role. */
  if (history->legacy_format) {
    return true;
  }
  const struct config_recovery_file *old = &history->previous_menu;
  if (!old->digest.present || !config_blob_equal(previous_limine, current_menu) ||
      !digest_equal(&old->digest, &previous_limine->digest)) {
    return true;
  }
  /* The recorded digest proves the exact old menu, including any marked
   * legacy entry, without retaining an outgoing boot binding. */
  const struct install_boot_menu replaced = {NULL, NULL, true};
  if (!install_boot_entry_plan(current_menu, &replaced, disk_guid, pool_partition, &committed,
      &plan->menu, &pending)) {
    return false;
  }
  plan->available = !pending;
  if (!pending) {
    plan->state = INSTALL_PENDING_ACTIVATION;
  }
  plan->menu_changed = !pending && !config_blob_equal(&plan->menu, current_menu);
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
  bool ok = install_pending_inspect(request->record, request->current_menu,
      request->previous_limine, request->facts.disk_guid, request->pool_partition, &plan) && plan.available;
  if (!ok) {
    report->status = CALL_BAD_REQUEST;
  } else {
    ok = config_storage_complete_pending(request, &plan, report);
  }
  install_pending_destroy(&plan);
  return ok;
}
