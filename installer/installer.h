#ifndef USERSPACE_INSTALLER_H
#define USERSPACE_INSTALLER_H

#include "progress.h"

#include <disk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALL_MIB UINT64_C(1048576)
/* Coexistence accepts only 512-byte logical sectors; classic also accepts 4096. */
#define INSTALL_COEXIST_BLOCK_SIZE UINT64_C(512)
#define INSTALL_ESP_BYTES (UINT64_C(512) * INSTALL_MIB)
#define INSTALL_ESP_CONFIGURATION_MAX_BYTES (64u * 1024u)
/* The installed boot entries: normal, and rescue, which ignores the pool's
 * boot configuration. The menu waits so the rescue entry stays reachable. */
#define INSTALL_BOOT_INIT_OPTION "init=boot://boot-init.pxe"
#define INSTALL_RESCUE_OPTION "boot.default_config=1"
#define INSTALL_MENU_TIMEOUT_SECONDS 3
/* Ordinary programs live in this pool volume, one directory per revision. */
#define INSTALL_ESP_PARTITION 1
#define INSTALL_POOL_PARTITION 2
#define INSTALL_BIN_VOLUME "bin"
#define INSTALL_HOME_VOLUME "home"
#define INSTALL_PACKAGES_VOLUME "packages"
#define INSTALL_HTTP_WORK_VOLUME "http-work"
#define INSTALL_UNKNOWN_REVISION "unknown"
/* Bounds shared by the GPT reader and its complete partition projection. */
#define INSTALL_GPT_MAX_PARTITIONS 256u
#define INSTALL_GPT_TABLE_MAX_BYTES 65536u
#define INSTALL_GPT_NAME_UNITS 36u

struct install_disk {
  handle_t handle;
  struct disk_info info;
  uint64_t bytes;
};

struct install_gpt_partition {
  uint64_t entry, first_block, block_count, attributes;
  uint8_t type_guid[16], guid[16];
  uint16_t name[INSTALL_GPT_NAME_UNITS];
};

struct install_gpt {
  uint64_t first_usable, last_usable;
  uint32_t slot_count, partition_count;
  uint8_t disk_guid[16];
  struct install_gpt_partition partitions[INSTALL_GPT_MAX_PARTITIONS];
};

/* Read-only projection of both agreeing healthy GPT copies and protective MBR.
 * Every nonempty slot is represented in entry order; entry is one-based, and
 * names retain all UTF-16 code units without assuming termination or validity.
 * Caller owns the complete output, cleared on failure; reason is optional and
 * borrowed static text, NULL on success. No snapshot or mutation authority is
 * established, and absent/damaged GPT is refused rather than treated as blank. */
bool install_gpt_inspect(const struct install_disk *disk, struct install_gpt *gpt,
    const char **reason);

/* A source is a borrowed file handle, or owned memory when memory is set. */
struct install_source {
  handle_t handle;
  uint64_t bytes;
  uint8_t *memory;
};

struct install_layout {
  uint64_t esp_partition, pool_partition;
  uint64_t esp_start, esp_bytes;
  uint64_t pool_start, pool_bytes;
  uint64_t esp_attributes, pool_attributes;
  uint8_t disk_guid[16], esp_guid[16], pool_guid[16];
};

enum install_esp_state {
  INSTALL_ESP_VALID,
  INSTALL_ESP_REBUILD,
  INSTALL_ESP_REFUSED,
};

struct install_update {
  bool eligible, degraded;
  const char *reason;
  struct install_layout layout;
  char revision[64];
  uint64_t free_blocks;
};

struct install_consent {
  bool eligible, final_pool, degraded;
  uint32_t pools, volumes;
  uint8_t existing_guid[16];
  const char *reason;
  /* Owned, printable escaped volume labels; NULL when none were readable. */
  char *volume_labels;
};

/* All inputs are borrowed. Reads allow byte ranges; writes require native
 * logical-block alignment. Chunking never retries a failed operation. */
bool install_read_fingerprint_begin(void);
bool install_read_fingerprint_finish(char digest[65]);
void install_read_fingerprint_abort(void);

bool install_read(const struct install_disk *disk, uint64_t offset,
    void *bytes, size_t length);
bool install_write(const struct install_disk *disk, uint64_t offset,
    const void *bytes, size_t length);
bool install_zero(const struct install_disk *disk, uint64_t offset, uint64_t length);
bool install_source_read(const struct install_source *source, uint64_t offset,
    void *bytes, size_t length);
uint32_t install_crc32(const void *bytes, size_t length);
bool install_layout_plan(const struct install_disk *disk, struct install_layout *layout);
bool install_gpt_write(const struct install_disk *disk, const struct install_layout *layout);
void install_guid_text(const uint8_t guid[16], char text[37]);

/* Inspection performs no writes and retains no mounted pools. All committed
 * journal validation finishes before the metadata overlay is exposed. */
bool install_scan_consent(const struct install_disk *disk, bool read_the_room,
    struct install_consent *consent);
void install_consent_destroy(struct install_consent *consent);

/* Update inspection never mounts, replays or writes a pool. GPT/pool checks
 * must be followed by ESP inspection before offering a target. */
bool install_scan_update(const struct install_disk *disk, struct install_update *update);
/* Inspect exactly the selected native pool, independent of classic GPT layout.
 * No mount, replay or writes; previous active revision remains unknown. */
bool install_scan_pool_update(const struct install_disk *disk,
    const struct install_layout *layout, struct install_update *update);
/* Damaged ESP contents can be rebuilt only after healthy GPT/pool admission.
 * Failed reads, allocation, unsupported extents, a valid FAT12/FAT16 volume
 * and explicit foreign or invalid disk bindings refuse recovery. Unreadable
 * revision text remains unknown; nonvalid results also supply a borrowed,
 * static reason. */
enum install_esp_state install_esp_inspect(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason);
/* Pure fixed-menu binding veto shared with raw inspection. Missing canonical
 * normal/rescue entries remain replaceable; any foreign, repeated or invalid
 * mount.disk binding refuses. Bytes are counted and may contain no terminator.
 * Reason is optional borrowed static text, NULL when no repair is needed. */
bool install_esp_configuration_binding(const uint8_t disk_guid[16], const char *bytes,
    size_t size, const char **reason);

struct npfs_header;
/* The decoded, prevalidated header supplies the new pool identity and layout.
 * Creates only system's root and its empty regular SAFE_TO_WIPE marker. */
bool install_pool_format(const struct install_disk *disk,
    const struct install_layout *layout, const struct npfs_header *header,
    const uint8_t volume_id[16], int64_t created_ns, bool created_valid);

/* The FAT module writes only a new, fixed boot tree; it is not a FAT service.
 * Planning is read-only and rejects sources that cannot fit before mutation.
 * Passing both kernel/archive as NULL selects loader/config/revision bootstrap;
 * one NULL is invalid. Raw callers need classic erase or reviewed ESP rebuild. */
struct install_esp;
struct install_esp *install_esp_plan(const struct install_disk *disk,
    const struct install_layout *layout, const struct install_source *efi,
    const struct install_source *kernel, const struct install_source *archive,
    const char *configuration, size_t configuration_bytes,
    const char *revision, size_t revision_bytes, uint32_t serial);
bool install_esp_write(const struct install_esp *esp);
/* Formatter-derived space after the exact planned bootstrap/tree allocation. */
bool install_esp_capacity(const struct install_esp *esp,
    uint64_t *cluster_bytes, uint64_t *free_bytes);
/* Reopens the recorded boot paths through on-disk FAT/directory metadata and
 * compares their contents to the borrowed inputs, rather than planned offsets. */
bool install_esp_verify(const struct install_esp *esp);
void install_esp_destroy(struct install_esp *esp);

/* Complete program units that leave the boot archive: non-rescue root .pxe
 * files and root .pxb directories, sorted by name. */
struct install_program {
  char *name;
  uint64_t kind;
};

struct install_programs {
  struct install_program *entries;
  size_t count;
  size_t files;
  uint64_t bytes;
  bool totals_known;
};
bool install_programs_list(handle_t boot, struct install_programs *programs);
void install_programs_destroy(struct install_programs *programs);
bool install_archive_rescue_identity(const struct install_source *archive);
/* The rescue archive: newc without selected roots or their descendants, as
 * owned memory. Descendant matching respects component boundaries. Exactly one
 * normalized config/installed.lua is rebound to the selected pool as literal
 * data; an unchanged binding preserves its original record bytes. */
bool install_archive_filter(const struct install_source *archive,
    uint64_t pool_partition, struct install_programs *programs, struct install_source *rescue);
struct install_manifest;
/* Outside any raw claim on the pool: creates the bin volume if missing, writes
 * complete program trees into bin/REVISION, replacing an earlier partial copy,
 * syncs the pool and verifies names, kinds, structure, sizes and bytes. */
bool install_programs_write(const struct install_disk *disk, uint64_t pool_partition, handle_t boot,
    const struct install_programs *programs, const char *revision,
    const struct install_manifest *manifest, const char *manifest_text, size_t manifest_bytes);
/* Recursively removes revisions except KEEP and PREVIOUS (which may be NULL).
 * The caller skips this best-effort stage when the previous revision is unknown. */
bool install_programs_cleanup(const struct install_disk *disk, uint64_t pool_partition,
    const char *keep,
    const char *previous);

#endif
