#ifndef LIBC_DESCRIPTOR_H
#define LIBC_DESCRIPTOR_H

#include <abi/handle.h>
#include <abi/startup.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdio.h>

struct descriptor_mode {
  bool readable, writable, append, create, truncate;
};

/* Entries own their native handle and cursor. A FILE association owns neither;
 * close invalidates it before reuse. No entry pointer escapes this module. */
void descriptor_adopt_standard(enum startup_stream_index index, FILE *stream);
/* A NULL stream creates an entry without a FILE association. */
int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream);
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
/* The logical position, excluding bytes read ahead. */
long descriptor_tell(int descriptor);
/* Input fflush: drop file read-ahead so later reads refetch; keep pipe bytes. */
void descriptor_discard_input(int descriptor);

/* Return one owned file handle. Allocates path workspace; never truncates. */
enum call_status file_open_path(const char *path, uint64_t rights,
                               bool create, handle_t *handle);

#endif
