#ifndef LIBC_STREAM_H
#define LIBC_STREAM_H

#include <abi/handle.h>
#include <abi/syscall.h>
#include <stdio.h>
#include <stdint.h>

enum stream_kind { STREAM_FILE, STREAM_CONSOLE, STREAM_PIPE };

struct pyxis_file {
  struct pyxis_file *next;
  handle_t handle;
  uint64_t position;
  enum stream_kind kind;
  bool readable, writable, append;
  bool eof, error, closed, allocated;
  int open_error; /* A missing/failed startup grant is reported on first use. */
};

int libc_call_errno(enum call_status status);
int stream_error(FILE *stream, int error);
bool stream_ready(FILE *stream, bool writing);
/* Return one owned file handle. Allocates path workspace; never truncates. */
enum call_status stream_open_path(const char *path, uint64_t rights,
                                 bool create, handle_t *handle);

#endif
