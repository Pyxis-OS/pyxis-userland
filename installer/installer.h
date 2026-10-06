#ifndef USERSPACE_INSTALLER_H
#define USERSPACE_INSTALLER_H

#include <disk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALL_MIB UINT64_C(1048576)
#define INSTALL_ESP_BYTES (UINT64_C(512) * INSTALL_MIB)
/* The installed boot entries: normal, and rescue, which ignores the pool's
 * boot configuration. The menu waits so the rescue entry stays reachable. */
#define INSTALL_BOOT_INIT_OPTION "init=boot://boot-init.pxe"
#define INSTALL_RESCUE_OPTION "boot.default_config=1"
#define INSTALL_MENU_TIMEOUT_SECONDS 3

struct install_disk {
  handle_t handle;
  struct disk_info info;
  uint64_t bytes;
};

struct install_source {
  handle_t handle;
  uint64_t bytes;
};

struct install_layout {
  uint64_t esp_start, esp_bytes;
  uint64_t pool_start, pool_bytes;
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
};

struct install_consent {
  bool eligible, final_pool, degraded;
  uint32_t pools, volumes;
  const char *reason;
  /* Owned, printable escaped volume labels; NULL when none were readable. */
  char *volume_labels;
};

/* All inputs are borrowed. Reads allow byte ranges; writes require native
 * logical-block alignment. Chunking never retries a failed operation. */
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
/* Damaged ESP contents can be rebuilt only after healthy GPT/pool admission.
 * Failed reads, allocation, unsupported extents and explicit foreign or invalid
 * disk bindings refuse recovery. Unreadable revision text remains unknown;
 * nonvalid results also supply a borrowed, static reason. */
enum install_esp_state install_esp_inspect(const struct install_disk *disk,
    const struct install_layout *layout, char revision[64], const char **reason);

struct npfs_header;
/* The decoded, prevalidated header supplies the new pool identity and layout.
 * Creates only system's root and its empty regular SAFE_TO_WIPE marker. */
bool install_pool_format(const struct install_disk *disk,
    const struct install_layout *layout, const struct npfs_header *header,
    const uint8_t volume_id[16], int64_t created_ns, bool created_valid);

/* The FAT module writes only a new, fixed boot tree; it is not a FAT service.
 * Planning is read-only and rejects sources that cannot fit before mutation. */
struct install_esp;
struct install_esp *install_esp_plan(const struct install_disk *disk,
    const struct install_layout *layout, const struct install_source *efi,
    const struct install_source *kernel, const struct install_source *archive,
    const char *configuration, size_t configuration_bytes,
    const char *revision, size_t revision_bytes, uint32_t serial);
bool install_esp_write(const struct install_esp *esp);
/* Reopens the recorded boot paths through on-disk FAT/directory metadata and
 * compares their contents to the borrowed inputs, rather than planned offsets. */
bool install_esp_verify(const struct install_esp *esp);
void install_esp_destroy(struct install_esp *esp);

#endif
