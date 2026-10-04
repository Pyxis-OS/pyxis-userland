#ifndef USERSPACE_INSTALLER_H
#define USERSPACE_INSTALLER_H

#include <disk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALL_MIB UINT64_C(1048576)
#define INSTALL_ESP_BYTES (UINT64_C(512) * INSTALL_MIB)

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

/* Inspection performs no writes and retains no mounted pools. All committed
 * journal validation finishes before the metadata overlay is exposed. */
bool install_scan_consent(const struct install_disk *disk, bool read_the_room,
    struct install_consent *consent);
void install_consent_destroy(struct install_consent *consent);

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
    const char *configuration, size_t configuration_bytes, uint32_t serial);
bool install_esp_write(const struct install_esp *esp);
/* Reopens the recorded boot paths through on-disk FAT/directory metadata and
 * compares their contents to the borrowed inputs, rather than planned offsets. */
bool install_esp_verify(const struct install_esp *esp);
void install_esp_destroy(struct install_esp *esp);

#endif
