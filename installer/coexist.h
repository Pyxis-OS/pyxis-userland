#ifndef USERSPACE_INSTALLER_COEXIST_H
#define USERSPACE_INSTALLER_COEXIST_H

#include "installer.h"

struct install_coexist {
  struct install_gpt gpt;
  uint32_t esp_count, pool_count;
  struct disk_partition_selection esps[INSTALL_GPT_MAX_PARTITIONS];
  struct disk_partition_selection pools[INSTALL_GPT_MAX_PARTITIONS];
  /* Entire largest MiB-aligned gap and first unused slot, with no new GUID.
   * Zero block_count or partition means no usable gap or unused slot. */
  struct disk_partition_selection gap;
  uint64_t default_bytes;
};

/* Caller owns the complete observation, including the GPT projection for
 * Review. False clears it and reports inspection failure. True preserves all
 * ESP/pool choices even without a new-install candidate; reason then describes
 * missing ESP, gap or slot. Multiple ESPs require explicit UI selection. No
 * existing pool is assumed owned, mounted or eligible for Update. */
bool install_coexist_inspect(const struct install_disk *disk,
    struct install_coexist *facts, const char **reason);

/* Pure proposal from a successful inspection above; keep facts unchanged.
 * size_bytes zero selects min(64 GiB, largest aligned gap).
 * Explicit size is nonzero whole MiB, bounded by that gap. The caller supplies
 * a nonzero GUID distinct from every existing partition. Failure clears the
 * selection; reason is optional borrowed static text, NULL on success.
 * Pool format capacity and selected ESP eligibility are checked by the caller.
 * No snapshot, ownership or mutation authority follows from these facts. */
bool install_coexist_choose(const struct install_coexist *facts,
    const uint8_t pool_guid[16], uint64_t size_bytes,
    struct disk_partition_selection *selection, const char **reason);

#endif
