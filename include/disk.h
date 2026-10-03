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

/* One exact, nonzero transfer of at most DISK_IO_MAX_BYTES. Device alignment
 * and bounds are checked by the native operation. READ returns length bytes
 * only on success; failed reads may have touched the caller's buffer. WRITE
 * needs a live claim and captures its input; success is not durability. No
 * wrapper chunks requests or retries mutations. Malformed replies are outcome
 * unknown. All buffers are borrowed until the call returns. */
enum call_status disk_read(handle_t disk, uint64_t offset, void *bytes, size_t length);
enum call_status disk_write(handle_t disk, uint64_t offset, const void *bytes, size_t length);
enum call_status disk_flush(handle_t disk);

/* Flushes, rescans GPT, then ends this object's raw claim on every alias.
 * Failure reports actual progress, not rollback. The owned handle stays open.
 * Final close also releases through worker cleanup. */
enum call_status disk_release(handle_t disk);

/* After release, open a read-only npfs root through this disk's MOUNT right.
 * Requires LOOKUP and accepts read/observation directory rights only. Success
 * returns an owned root independent of disk lifetime; failure clears it. */
enum call_status disk_open_volume(handle_t disk, uint64_t partition,
    const char *name, uint64_t rights, handle_t *root);

#endif
