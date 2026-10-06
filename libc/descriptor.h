#ifndef LIBC_DESCRIPTOR_H
#define LIBC_DESCRIPTOR_H

#include <abi/handle.h>
#include <abi/startup.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

struct descriptor_mode {
  bool readable, writable, append, create, truncate, exclusive;
};

/* Entries own their native handle and cursor. A FILE association owns neither;
 * close invalidates it before reuse. No entry pointer escapes this module. */
void descriptor_adopt_standard(enum startup_stream_index index, FILE *stream);
/* A NULL stream creates an entry without a FILE association. */
int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream);
/* Reserve before exclusive creation, remove the name through the held parent,
 * then publish the FILE association. Parent has CREATE/REMOVE and file rights. */
int descriptor_tmpfile(FILE *stream, handle_t parent);
/* Borrow the actual associated handle; never changes input, position or flags. */
int descriptor_stream(FILE *stream, struct startup_stream *binding);
bool descriptor_ready(int descriptor, bool writing);
int descriptor_close(int descriptor);
void descriptor_finish(void);

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
int descriptor_write(int descriptor, const void *buffer, size_t size, size_t *written);
int descriptor_seek(int descriptor, long offset, int origin);
/* Files only; keeps the position and discards read-ahead before RESIZE. */
int descriptor_resize(int descriptor, uint64_t size);
/* Files only; native SYNC, which needs WRITE. */
int descriptor_sync(int descriptor);
/* Type and size of the descriptor's object, as fstat reports them. */
int descriptor_stat(int descriptor, struct stat *result);
/* The logical position, excluding bytes read ahead. */
long descriptor_tell(int descriptor);
/* Input fflush: drop file read-ahead so later reads refetch; keep pipe bytes. */
void descriptor_discard_input(int descriptor);

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

#endif
