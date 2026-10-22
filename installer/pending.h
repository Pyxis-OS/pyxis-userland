#ifndef USERSPACE_INSTALLER_PENDING_H
#define USERSPACE_INSTALLER_PENDING_H

#include "publication.h"

/* Pure evidence for one explicitly reviewed completion of an existing committed
 * readiness record. No new payload attempt, record or retention generation. */
enum install_pending_state {
  INSTALL_PENDING_NONE, INSTALL_PENDING_REFUSED,
  INSTALL_PENDING_ACTIVATION, INSTALL_PENDING_MENU_CURRENT
};

struct install_pending_plan {
  enum install_pending_state state;
  bool available, menu_changed, legacy_active;
  struct config_record record;
  struct config_blob menu;
};

/* Initialize PLAN with {0}; destroy releases its owned rendered menu. A current
 * menu with edited configuration is MENU_CURRENT but not available for cleanup;
 * ordinary reconciliation remains possible when no obsolete artifacts remain. */

bool install_pending_inspect(const struct config_blob *record,
    const struct config_blob current[CONFIG_ALL_FILES],
    const struct config_blob *previous_limine, const uint8_t disk_guid[16],
    uint64_t pool_partition, const struct config_file_digest legacy[INSTALL_BOOT_PAYLOADS],
    struct install_pending_plan *plan);
void install_pending_destroy(struct install_pending_plan *plan);

/* Borrowed frozen snapshots and already admitted native target. FALLBACK_READY
 * records the trusted caller's completed static fallback verification; it does
 * not confer authority. The caller must obtain fresh Review consent and perform
 * initial snapshot revalidation under the exclusive scoped writable view. */
struct install_pending_request {
  handle_t esp_root, workspace;
  struct config_storage_view view;
  struct config_target_facts facts;
  uint64_t pool_partition; /* The entries' mount.pool=. */
  struct mount_state_reply expected_target;
  const struct config_blob *record, *current, *previous_limine;
  struct config_file_digest legacy[INSTALL_BOOT_PAYLOADS];
  bool fallback_ready;
};
bool install_pending_complete(const struct install_pending_request *request,
    struct config_storage_report *report);

#endif
