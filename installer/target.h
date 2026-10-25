#ifndef USERSPACE_INSTALLER_TARGET_H
#define USERSPACE_INSTALLER_TARGET_H

#include "installer.h"
#include "plan_record.h"
#include "review.h"

enum install_target_review_kind {
  INSTALL_TARGET_REVIEW_NONE, INSTALL_TARGET_REVIEW_ERASE, INSTALL_TARGET_REVIEW_APPEND,
};

struct install_target {
  handle_t disks, clock; /* Borrowed. */
  handle_t review_handle, rebuild_review; /* Owned, unconsumed Apply authority. */
  enum install_target_review_kind review_kind;
  struct install_disk disk; /* Owned whole-device RO handle, retained if close fails. */
  struct install_plan_record record;
  struct install_review review;
  struct disk_partition_selection pool, esp;
  /* A fresh recheck whose cleanup failed remains owned and blocks another. */
  struct install_target *pending_recheck;
  enum call_status status;
  const char *reason;
  bool initialized, prepared;
};

/* Start with {0}; output consent also starts empty and remains caller-owned
 * on failure. Record freezes choices, but source/target hashes may be unset.
 * No mutation or RW admission. Review lists both GPT copies, every known entry
 * and every known ESP's EFI entries, then drains its RO views. Selected native
 * ESP evidence remains in review. Success closes disk.handle and retains only
 * the reviewed Apply grants and report; failure retains every unclosed handle.
 * deadline_ns bounds cleanup only. */
bool install_target_prepare(struct install_target *target, handle_t disks, handle_t clock,
    const struct disk_info *info, const struct install_plan_record *record,
    uint64_t deadline_ns, struct install_consent *consent,
    uint64_t *free_blocks, char target_hash[65]);

/* Acquires fresh RO data and Review, recomputes the full target fingerprint and
 * compares it without replacing or consuming the original Apply authority.
 * A failed recheck disables this preparation. */
bool install_target_revalidate(struct install_target *target, uint64_t deadline_ns);

/* Drain Review references, then close owned disk/review handles. Failed closes
 * stay owned; false preserves the object for later cleanup. No automatic retry
 * of admission, reads or mutations. Only Review's BUSY retirement is retried. */
bool install_target_destroy(struct install_target *target, uint64_t deadline_ns);

#endif
