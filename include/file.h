#ifndef USERSPACE_FILE_H
#define USERSPACE_FILE_H

#include <abi/file.h>
#include <abi/handle.h>
#include <abi/syscall.h>
#include <stddef.h>

/* Dispatch native and exported FILE capabilities through their held interface.
 * Exported files require CALL transport. Return operation/transport status;
 * output values are cleared on failure. Callers provide
 * valid output storage and valid byte buffers for nonzero transfers. Output
 * counts are written after byte transfers if they alias the buffer. SIZE
 * accepts either READ or WRITE. Malformed mutation replies report
 * CALL_OUTCOME_UNKNOWN: they cannot prove that the operation had no effect.
 * Delivered transport failures on mutations are equally uncertain. */
enum call_status file_size(handle_t file, uint64_t *size);

/* Sample the held object's optional metadata with READ or WRITE. Validity bits
 * are independent; OBJECT implies DOMAIN. Different valid domains prove
 * distinctness even without object IDs. Hold both references during comparison.
 * An unsupported INFO falls back to SIZE only; other failures clear *info.
 * Neither identity nor mtime freezes contents, and mtime is not a change count. */
enum call_status file_info(handle_t file, struct file_info_reply *info);

/* Reads up to min(capacity, FILE_READ_MAX_BYTES) bytes at an explicit offset
 * in one call. Short reads are allowed; zero with nonzero capacity means EOF.
 * Zero capacity does not touch bytes. Requires READ. No shared seek position
 * is changed. */
enum call_status file_read(handle_t file, uint64_t offset, void *bytes, size_t capacity,
                            size_t *read);

/* Requires WRITE independently of READ. One call copies at most
 * FILE_WRITE_MAX_BYTES and nonempty success reports 1..that limit bytes;
 * advance offset/source by that count before submitting the remaining suffix.
 * Rejects overflow of offset + size before limiting the submitted chunk.
 * Failure reports no known count for this call, not a promise of no changes;
 * do not automatically retry CALL_OUTCOME_UNKNOWN. Earlier successful calls
 * retain their progress. Beyond-EOF gaps read as zero. Zero size validates
 * authority/arguments but neither touches bytes nor extends the file. */
enum call_status file_write(handle_t file, uint64_t offset, const void *bytes,
                             size_t size, size_t *written);

/* Requires WRITE. Growth exposes zeroes, shrinking discards the tail.
 * CALL_OUTCOME_UNKNOWN means the resize may have happened. RAM backing retains
 * its unchanged-on-failure guarantee; resizing to zero releases RAM storage. */
enum call_status file_resize(handle_t file, uint64_t size);

/* Requires WRITE. Flushes this file's data and metadata to its backing store.
 * RAM backing succeeds without work. A submitted call with an untrustworthy
 * reply reports CALL_OUTCOME_UNKNOWN; callers must not retry automatically. */
enum call_status file_sync(handle_t file);

#endif
