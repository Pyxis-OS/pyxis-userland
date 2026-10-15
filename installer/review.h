#ifndef USERSPACE_INSTALLER_REVIEW_H
#define USERSPACE_INSTALLER_REVIEW_H

#include "installer.h"

enum install_review_copy_status {
  INSTALL_REVIEW_GPT_ABSENT, INSTALL_REVIEW_GPT_VALID, INSTALL_REVIEW_GPT_DAMAGED,
  INSTALL_REVIEW_GPT_UNSUPPORTED, INSTALL_REVIEW_GPT_UNAVAILABLE,
};

struct install_review_gpt {
  bool mbr_read, mbr_protective, mbr_zero, copies_agree;
  enum install_review_copy_status status[2];
  /* Only VALID copies supply known entries. Invalid copies stay cleared. */
  struct install_gpt maps[2];
};

/* Shared consent parser, retaining per-copy facts even when GPT is damaged.
 * False means invalid inputs/allocation failure; I/O is reported per copy.
 * No claim that unknown entries are absent follows from an invalid copy. */
bool install_review_gpt_read(const struct install_disk *disk,
    struct install_review_gpt *facts, const char **reason);

struct install_review_esp {
  struct disk_partition_selection selection;
  struct mount_state_reply observed;
  bool identity_matches, available, efi_absent, retired, retirement_attempted;
  enum call_status admission_status, observe_status, content_status, retire_status;
  /* Owned only when cleanup remains pending. Never discard these handles. */
  handle_t root, efi, control;
};

struct install_review {
  char *text;
  size_t text_bytes, text_capacity;
  bool prepared, metadata_complete, blank, cache_matches;
  struct install_review_gpt *gpt;
  struct install_review_esp *esps;
  size_t esp_count;
  const char *reason;
};

/* Borrow a caller-owned kernel review READ handle in disk. Always reread its
 * retained GPT facts; optional cache is compared only, never trusted as authority.
 * issuer needs MOUNT_READ_ONLY. deadline_ns bounds RETIRE/BUSY drainage through
 * clock READ; content/I/O operations are never retried. Text is bounded by the
 * existing plan-record budget, escapes every untrusted name, and includes only
 * stable target facts, statuses and directory listings, without handle IDs/time.
 * GPT damage and unavailable FAT are reportable observations, not eligibility
 * policy. A true result prepares the report and retires all admitted RO views.
 * False still returns owned partial data and pending references for cleanup.
 * An uncertain admission with no returned control remains explicitly uncertain;
 * cleanup cannot certify retirement of an instance it cannot identify.
 * Use a zero-initialized output, then cleanup/destroy before preparing another. */
bool install_review_prepare(const struct install_disk *disk, handle_t issuer,
    handle_t clock, uint64_t deadline_ns, const struct install_gpt *cache,
    struct install_review *review);

/* Only BUSY retirement is retried, bounded by caller's deadline. All reference
 * and control failures remain owned in review. Destroy refuses pending handles
 * and preserves the complete object; it never silently abandons exclusion. */
bool install_review_cleanup(struct install_review *review, handle_t clock, uint64_t deadline_ns);
bool install_review_destroy(struct install_review *review);

#endif
