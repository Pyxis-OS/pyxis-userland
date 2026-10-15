#ifndef USERSPACE_INSTALLER_ESP_VIEW_H
#define USERSPACE_INSTALLER_ESP_VIEW_H

#include <abi/directory.h>
#include <disk.h>
#include <mount.h>

#define INSTALL_ESP_VIEW_RIGHTS (DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_ENUMERATE | \
    DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_WRITE_FILES | \
    DIRECTORY_RIGHT_REMOVE)

/* Initialize to zero. One admission attempt owns every returned handle even
 * after validation fails. Do not reset while handles or uncertain admission
 * remain. Children do not inherit this process's interval ownership. */
struct install_esp_view {
  handle_t root, control;
  uint64_t interval_id, interval_deadline_ns;
  struct mount_state_reply expected;
  enum call_status failure_status, cleanup_status;
  bool attempted, admitted, retired;
};

/* Optional RO evidence compares only immutable GPT/geometry facts; its domain
 * and mutable flags need not match the new RW instance. Root rights are exact
 * subsets of INSTALL_ESP_VIEW_RIGHTS. Errors never retry admission or discard
 * owned handles. Native FAT geometry restrictions remain native admission's. */
enum call_status install_esp_view_open(struct install_esp_view *view, handle_t disks,
    const struct disk_partition_selection *selection,
    const struct mount_state_reply *expected_ro, uint64_t root_rights);
/* BEGIN then CHECK immediately, in this process. A failed CHECK retains the
 * known interval for END. Any failure stops further BEGIN attempts on the view. */
enum call_status install_esp_view_begin_checked(struct install_esp_view *view);
enum call_status install_esp_view_end(struct install_esp_view *view);
/* Caller first closes all staged file/directory handles. END, root close,
 * RETIRE and control close occur in that order. Only RETIRE BUSY is retried,
 * until the absolute monotonic deadline using clock READ|SLEEP. Other cleanup
 * failures preserve remaining handles/IDs and are not automatically retried.
 * Retirement may seal native FAT's owned dirty mark; no staging is performed. */
enum call_status install_esp_view_close_and_retire(struct install_esp_view *view,
    handle_t clock, uint64_t deadline_ns);

#endif
