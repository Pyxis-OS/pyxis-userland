#ifndef USERSPACE_INSTALLER_BOOT_STORAGE_H
#define USERSPACE_INSTALLER_BOOT_STORAGE_H

#include "boot_record.h"
#include <abi/handle.h>
#include <abi/syscall.h>

/* Internal DTO, not an ABI or caller-authored proof. The selected-view issuer's
 * bound OBSERVE facts must supply it. Missing facts refuse publication/replay.
 * The primary re-observes complete facts before calling publication; storage
 * independently verifies its retained control/domain/extent and actual grants. */
struct config_target_facts {
  bool valid;
  uint64_t domain, device, partition, first_block, block_count;
  uint64_t device_block_size, device_block_count, partition_attributes;
  uint8_t disk_guid[16], partition_guid[16], partition_type[16];
};

struct config_storage_view {
  handle_t config_parent, limine_parent, control, clock;
};

struct config_blob {
  unsigned char *bytes;
  size_t size;
  struct config_file_digest digest;
};

/* Initialize blobs with {0}; setters/reads replace owned bytes. */

enum config_publish_phase {
  CONFIG_PHASE_CHECK, CONFIG_PHASE_STAGE, CONFIG_PHASE_VERIFY,
  CONFIG_PHASE_EXCLUSIVE, CONFIG_PHASE_PREPARED, CONFIG_PHASE_COMMITTED, CONFIG_PHASE_ACTIVATE, CONFIG_PHASE_CLEANUP, CONFIG_PHASE_DONE
};

struct config_storage_report {
  enum call_status status;
  enum config_publish_phase phase;
  char artifact[CONFIG_ARTIFACT_NAME_BYTES];
  struct config_record record;
  bool committed;
  bool boot_activated;
  bool boot_activation_pending;
  bool boot_cleanup_skipped;
};

struct install_staged_boot;
struct install_pending_request;
struct install_pending_plan;

/* Installer-only explicitly reviewed completion; retains the existing committed
 * record and its retention generation. See installer/pending.h. */
bool config_storage_complete_pending(const struct install_pending_request *request,
    const struct install_pending_plan *plan, struct config_storage_report *report);

bool config_storage_guard(const struct config_storage_view *view,
    const struct config_target_facts *facts, uint64_t interval,
    struct config_storage_report *report);

/* Read-only preflight, repeated by the publisher before staging and again
 * under its interval. Existing malformed/prepared records refuse. */
bool config_storage_check(const struct config_storage_view *view,
    const struct config_target_facts *facts, const struct config_blob *expected_menu,
    const struct config_blob *expected_record, bool menu_changed,
    struct config_storage_report *report);

/* EXPECTED and SELECTED are the current and the new owned menu, borrowed and
 * immutable. Staged payload handles remain retained through the final checks;
 * the record commits before the menu name moves. The transaction is the boot
 * attempt, a fresh 32-hex name: collisions stop; no retry/rollback/resume. */
bool config_storage_publish_boot(const struct config_storage_view *view,
    const struct config_target_facts *facts, const struct config_blob *expected,
    const struct config_blob *expected_record, const struct config_blob *selected,
    const char *transaction, const struct install_staged_boot *boot,
    struct config_storage_report *report);

void config_blob_destroy(struct config_blob *blob);
bool config_blob_set(struct config_blob *blob, const void *bytes, size_t size);
bool config_blob_equal(const struct config_blob *left, const struct config_blob *right);
bool config_target_equal(const struct config_target_facts *left,
    const struct config_target_facts *right);
bool config_target_valid(const struct config_target_facts *facts);
/* A missing record is successful and represented by digest.present == false.
 * All returned bytes are owned, bounded, NUL-terminated and hashed exactly. */
bool config_storage_read_record(const struct config_storage_view *view,
    struct config_blob *blob, struct config_storage_report *report);

#endif
