#ifndef USERSPACE_DISK_H
#define USERSPACE_DISK_H

#include <abi/disk.h>
#include <abi/mount.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Borrow the inventory service. NOT_FOUND marks the end; incomplete inventory
 * is an error. INFO outputs are cleared on failure. IDs last for this boot. */
enum call_status disks_enumerate(handle_t disks, uint64_t index, struct disk_info *info);
enum call_status disk_get_info(handle_t disk, struct disk_info *info);

/* Success returns an owned disk handle; failure clears it. READ_WRITE claims
 * exclusive raw access and may fail BUSY for retained pools or another claim.
 * Malformed replies are outcome unknown; never retry an uncertain open. */
enum call_status disks_open(handle_t disks, uint64_t id, uint64_t access, handle_t *disk);

/* Retain a reviewed device's geometry and GPT framing without a write claim.
 * INFO supplies the observed identity; a zero GPT GUID permits damaged/absent
 * GPT review. Success owns a single-use review; failure clears it. */
enum call_status disks_review_erase(handle_t disks, const struct disk_info *info,
    handle_t *review);
enum call_status disks_review_append(handle_t disks, const struct disk_info *info,
    handle_t *review);

/* Consume erase review after exact DISK_ERASE_CONFIRMATION consent. Success
 * owns a separate claimed writer; failure clears it. Never retry an uncertain
 * begin or append: the review may already be consumed. */
enum call_status disk_begin_erase(handle_t review, const char *confirmation,
    handle_t *writer);

/* Open exactly the reviewed GPT extent. OPEN_POOL admits only the native pool
 * type; OPEN_PARTITION needs separate raw qualification authority. Success
 * owns an already claimed, permanently partition-bound disk. Failure clears
 * the output. Malformed replies report outcome unknown; never retry an open.
 * PARTITION_INFO clears selection on failure. */
enum call_status disks_open_pool(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *disk);
enum call_status disks_open_partition(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *disk);
enum call_status disk_partition_info(handle_t disk, uint64_t entry,
    struct disk_partition_selection *selection);

/* Explicit classic recovery authority. Review binds the selected ESP and exact
 * GPT snapshot; begin consumes it once with the exact confirmation "update".
 * The returned writer is ESP-only, with no mount or later reclaim authority.
 * Healthy ESPs use native FAT views. Failure clears the output; never retry an
 * uncertain reply automatically. */
enum call_status disks_review_esp(handle_t disks,
    const struct disk_partition_selection *selection, handle_t *review);
enum call_status disk_begin_esp_rebuild(handle_t review, const char *confirmation,
    handle_t *writer);

/* Append one proposed native-pool partition through single-use review.
 * Full worker failures retain validated phase/identity evidence and no handle;
 * early failures leave PREPARE, UINT64_MAX failed block and an invalid handle.
 * Malformed replies leave those defaults and report outcome unknown. */
enum call_status disk_append_pool(handle_t review,
    const struct disk_partition_selection *selection, struct disk_append_reply *reply);

/* Admit a selected GPT FAT partition through mount-only inventory authority,
 * without a raw handle or claim. RW authority also permits RO admission.
 * A nonzero expected_disk_guid pins the configured selection under the worker;
 * NULL selects only the explicit boot-scoped ID. No guessed fallback.
 * Success owns root/control; failure clears both. Final close does not retire.
 * A malformed reply reports outcome unknown and must not be retried. */
enum call_status disks_open_fat32(handle_t disks, uint64_t id, uint64_t partition,
    uint64_t access, uint64_t rights, const uint8_t expected_disk_guid[16],
    struct mount_fat32_reply *mount);

/* One exact, nonzero transfer of at most DISK_IO_MAX_BYTES. Device alignment
 * and bounds are checked by the native operation. READ returns length bytes
 * only on success; failed reads may have touched the caller's buffer. WRITE
 * needs a live claim and captures its input; success is not durability. No
 * wrapper chunks requests or retries mutations. Malformed replies are outcome
 * unknown. All buffers are borrowed until the call returns. */
enum call_status disk_read(handle_t disk, uint64_t offset, void *bytes, size_t length);
enum call_status disk_write(handle_t disk, uint64_t offset, const void *bytes, size_t length);
enum call_status disk_flush(handle_t disk);

/* Flushes, rescans GPT for a whole-disk claim, then ends this object's raw
 * claim on every alias. Failure reports actual progress, not rollback. The owned handle stays open.
 * Final close also releases through worker cleanup. */
enum call_status disk_release(handle_t disk);

/* Outside any raw claim on PARTITION, open an npfs root through this disk's
 * MOUNT right. Requires LOOKUP. Mutation rights also need the disk's WRITE
 * right, which only READ_WRITE opening grants. Success returns an owned root
 * independent of disk lifetime; failure clears it. */
enum call_status disk_open_volume(handle_t disk, uint64_t partition,
    const char *name, uint64_t rights, handle_t *root);

/* Admit the selected FAT32 GPT partition through MOUNT authority. Rights are
 * exact DIRECTORY_CONTENT_RIGHTS bits. access fixes RO/RW instance mode; RW
 * also needs disk WRITE and a released raw claim. Success owns
 * both root and bound control; failure clears both. The control can reopen only
 * this instance and explicitly retire it after every data reference drains.
 * Closing either handle does not retire the instance. Malformed replies are
 * outcome unknown; do not retry an uncertain admission automatically. */
enum call_status disk_open_fat32(handle_t disk, uint64_t partition,
    uint64_t access, uint64_t rights, struct mount_fat32_reply *mount);

/* Needs MOUNT and WRITE. Appends a live volume NAME with an empty root to the
 * npfs pool in PARTITION, durably. A taken name returns ALREADY_EXISTS. */
enum call_status disk_create_volume(handle_t disk, uint64_t partition, const char *name);

/* Needs WRITE, on an object whose claim was released. PARTITION zero claims the
 * whole disk; a GPT entry claims only that partition, which must not be mounted,
 * while other partitions' pools stay mounted. Writes are confined to the claim,
 * and releasing a partition claim flushes without a GPT rescan. */
enum call_status disk_claim(handle_t disk, uint64_t partition);

#endif
