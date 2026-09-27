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
int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream);
bool descriptor_ready(int descriptor, bool writing);
int descriptor_close(int descriptor);
void descriptor_finish(void);

/* One backend transfer, without FILE indicators or fill/retry loops. A zero
 * read is EOF; a nonempty zero write is an error. Failure sets errno. */
int descriptor_read(int descriptor, void *buffer, size_t capacity, size_t *read);
int descriptor_write(int descriptor, const void *buffer, size_t size, size_t *written);
int descriptor_seek(int descriptor, long offset, int origin);
long descriptor_tell(int descriptor);

/* Return one owned file handle. Allocates path workspace; never truncates. */
enum call_status file_open_path(const char *path, uint64_t rights,
                               bool create, handle_t *handle);

#endif
