#include <environment.h>
#ifndef LIBC_DESCRIPTOR_H
#define LIBC_DESCRIPTOR_H

#include <output.h>

#include <abi/handle.h>
#include <abi/startup.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdio.h>
#include <abi/describe.h>
#include <sys/stat.h>

struct descriptor_object;

struct descriptor_mode {
  bool readable, writable, append, create, truncate, exclusive;
};

/* Entries reference an open object owning handle, access, cursor and read-ahead.
 * A FILE association owns neither; close invalidates it before slot reuse.
 * No entry pointer escapes this module; open objects stay stable on growth.
 * Admitted operations retain storage only. Final alias CLOSE releases native
 * authority immediately, outside exclusion; storage outlives admitted calls. */
void descriptor_adopt_standard(enum environment_stream_index index, FILE *stream,
    struct pyxis_stdio_binding binding, uint64_t identity);
/* A NULL stream creates an entry without a FILE association. */
int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream);
/* Native FILE admission occurs in the captured route before mutable OPEN work. */
int descriptor_open_native_file(const char *path, const struct descriptor_mode *mode);
/* Reserve before exclusive creation, remove the name through the held parent,
 * then publish the FILE association. Parent has CREATE/REMOVE and file rights. */
int descriptor_tmpfile(FILE *stream, handle_t parent);
/* Borrow the actual associated handle; never changes input, position or flags. */
int descriptor_stream(FILE *stream, struct pyxis_stdio_binding *binding);
/* Validate mode/access and reject an existing association before changing any
 * state. Success takes ownership for fclose and retains cursor/read-ahead. */
int descriptor_associate(int descriptor, const struct descriptor_mode *mode, FILE *stream);
bool descriptor_ready(int descriptor, bool writing);
int descriptor_close(int descriptor);
/* One CLOSE attempt for an owned temporary handle; same reply/error validation
 * as descriptor close. Never retries uncertain release; teardown owns recovery. */
int descriptor_release_handle(handle_t handle);
int descriptor_finish(void);

/* No FILE indicators or fill/retry loops. A zero read is EOF; a nonempty zero
 * write is an error. Failure sets errno.
 *
 * Reads first return bytes already read ahead, without a backend call. The
 * exact read then performs one backend transfer of at most capacity bytes, as
 * read() and fread_some require. The buffered read, used only by fread and the
 * functions built on it, may instead fetch up to BUFSIZ bytes from a file or
 * pipe in one transfer and keep the excess. Consoles are never read ahead.
 * Read-ahead is private to this process and never accompanies a delegated
 * handle. Writes discard file read-ahead; seeks do so only after validation. */
int descriptor_read(int descriptor, void *buffer, size_t capacity, size_t *read);
int descriptor_read_buffered(int descriptor, void *buffer, size_t capacity, size_t *read);
/* read() semantics: as descriptor_read, but a regular file keeps transferring
 * until capacity bytes arrive or the file ends, since each native transfer is
 * under 4 KiB. A failure after progress returns the bytes already read; the
 * error recurs on the next call. Pipes and consoles keep one short transfer. */
int descriptor_read_complete(int descriptor, void *buffer, size_t capacity, size_t *read);
int descriptor_write(int descriptor, const void *buffer, size_t size, size_t *written);
/* File-only explicit offsets, without changing the private position. Read
 * bypasses read-ahead; nonempty write discards it before dispatch. Both snapshot
 * the held handle and retain object storage across every blocking call. */
int descriptor_pread(int descriptor, void *buffer, size_t size, uint64_t offset,
    size_t *read);
int descriptor_pwrite(int descriptor, const void *buffer, size_t size, uint64_t offset,
    size_t *written);
int descriptor_seek(int descriptor, long offset, int origin);
/* Files only; keeps the position and discards read-ahead before RESIZE. */
int descriptor_resize(int descriptor, uint64_t size);
/* Files only; native SYNC, which needs WRITE. */
int descriptor_sync(int descriptor);
/* Type, size and available optional metadata, without changing the cursor. */
int descriptor_stat(int descriptor, struct stat *result);
/* Shared native INFO conversion, preserving optional validity and size range. */
enum call_status libc_stat_result(const struct file_info_reply *info, mode_t mode,
    struct stat *result);
/* Shared held-FILE conversion for stat and descriptor_stat. */
enum call_status libc_file_stat(handle_t file, struct stat *result);
/* 1 for a console stream, the only terminal; files and pipes fail with ENOTTY. */
int descriptor_terminal(int descriptor);
/* The logical position, excluding bytes read ahead. */
long descriptor_tell(int descriptor);
/* Include FILE-owned queued output. Pending append output uses held file SIZE,
 * without moving the cursor; every position/addition fits the stdio long range. */
long descriptor_tell_output(int descriptor, size_t pending);
/* Input fflush: drop file read-ahead so later reads refetch; keep pipe bytes. */
void descriptor_discard_input(int descriptor);

/* Validate WRITE access and flush only this descriptor's associated FILE output.
 * Preserve input and leave streams without pending output unchanged. */
int libc_stdio_flush_descriptor(int descriptor);
int libc_stdio_stage_close_descriptor(int descriptor, struct descriptor_object *expected);
int libc_stdio_close_descriptor(int descriptor, struct descriptor_object *expected,
    uint64_t generation);



/* Return one owned file handle. Allocates path workspace; never truncates. */
enum call_status file_open_path(const char *path, uint64_t rights,
                               bool create, handle_t *handle);
enum call_status file_create_path(const char *path, uint64_t rights, handle_t *handle);
/* Six random filename bytes, no terminator. Native random and clock required. */
#define TEMPORARY_NAME_LENGTH 6
#define TEMPORARY_CREATE_ATTEMPTS 128
enum call_status file_temporary_name(char *suffix);
/* Return one owned directory handle with exactly rights, resolved like a file. */
enum call_status directory_open_path(const char *path, uint64_t rights, handle_t *handle);
/* Native admission retaining all actual directory rights and requiring ENUMERATE. */
enum call_status libc_directory_open_native(const char *path, handle_t *handle);

struct descriptor_object;
/* Hold storage only. Final alias CLOSE never waits for this reference. */
struct descriptor_object *descriptor_hold(int descriptor);
void descriptor_drop(struct descriptor_object *object);
bool descriptor_file_begin(struct descriptor_object *object);
void descriptor_file_end(struct descriptor_object *object);
static inline void descriptor_scope_end(struct descriptor_object **object)
{
  descriptor_drop(*object);
}
int descriptor_close_stream(FILE *stream, struct descriptor_object *object);
/* Caller has intentionally aborted under FILE exclusion. Skip submission and
 * the recorded producer failure; native grant-release failures still report. */
int descriptor_close_stream_abandon(FILE *stream, struct descriptor_object *object);
int descriptor_close_expected(int descriptor, struct descriptor_object *expected,
    uint64_t generation);
bool descriptor_object_ready(struct descriptor_object *object, bool writing);
int descriptor_object_stream(struct descriptor_object *object, struct pyxis_stdio_binding *binding);
/* Caller holds FILE exclusion. Copies eligible unread primary.
 * Success also holds descriptor read exclusion until descriptor_file_end. */
enum call_status descriptor_object_output_copy(struct descriptor_object *object,
    struct output_handoff_request *owned);
/* Pipe input only: 1 moves the next unread value to the caller, 0 means bytes
 * or end come next (including any non-pipe input), -1 sets errno. */
int descriptor_object_take_value(struct descriptor_object *object, handle_t *value);
int descriptor_object_read(struct descriptor_object *object, void *buffer, size_t size, size_t *read);
int descriptor_object_read_complete(struct descriptor_object *object, void *buffer, size_t size, size_t *read);
int descriptor_object_read_buffered(struct descriptor_object *object, void *buffer, size_t size, size_t *read);
int descriptor_object_write(struct descriptor_object *object, const void *buffer, size_t size, size_t *written);
int descriptor_object_pread(struct descriptor_object *object, void *buffer, size_t size, uint64_t offset, size_t *read);
int descriptor_object_pwrite(struct descriptor_object *object, const void *buffer, size_t size, uint64_t offset, size_t *written);
int descriptor_object_seek(struct descriptor_object *object, long offset, int origin);
int descriptor_object_resize(struct descriptor_object *object, uint64_t size);
int descriptor_object_sync(struct descriptor_object *object);
int descriptor_object_stat(struct descriptor_object *object, struct stat *result);
int descriptor_object_terminal(struct descriptor_object *object);
long descriptor_object_tell_output(struct descriptor_object *object, size_t pending);
long descriptor_object_tell(struct descriptor_object *object);
void descriptor_object_discard_input(struct descriptor_object *object);
long descriptor_seek_position(int descriptor, long offset, int origin);

/* Shared typed transaction state; ordinary streams return success from commit.
 * Abandon records a producer failure and never submits. */
struct pyxis_open_result;
int descriptor_open_result(const char *path, const struct descriptor_mode *mode,
    FILE *stream, struct pyxis_open_result *failure);
bool descriptor_object_transaction(struct descriptor_object *object);
bool descriptor_object_transaction_pending(struct descriptor_object *object);
int descriptor_object_commit(struct descriptor_object *object);
int descriptor_object_abandon(struct descriptor_object *object, int failure);
int descriptor_object_describe(struct descriptor_object *object, uint64_t flags,
    struct describe_reply *reply);
void descriptor_report_finalize_error(void);

#endif
