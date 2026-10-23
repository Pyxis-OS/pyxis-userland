#ifndef LIBC_STREAM_H
#define LIBC_STREAM_H

#include <stdio.h>
#include <wchar.h>
#include <pyxis/sync.h>

enum stream_orientation {
  STREAM_UNORIENTED = 0, STREAM_BYTE_ORIENTED = -1, STREAM_WIDE_ORIENTED = 1,
};

struct pyxis_file {
  struct pyxis_file *next;
  struct libc_mutex lock;
  /* Association admission cannot take the registry lock under table exclusion. */
  _Atomic uint32_t references;
  _Atomic bool closing;
  size_t operation_depth;
  uint64_t operation_owner;
  bool operation_cursor;
  struct descriptor_object *operation; /* Storage-only hold for the outer call. */
  int descriptor; /* Non-owning association; descriptor close sets this to -1. */
  bool readable, writable; /* FILE selection can narrow descriptor access. */
  bool eof, error, closed, allocated;
  bool transaction_read_blocked; /* Only a refused pre-commit read, never a failed producer. */
  bool has_pushback; /* One ungetc byte, returned before any descriptor input. */
  unsigned char pushback;
  enum stream_orientation orientation;
  mbstate_t wide_input;
  bool has_wide_pushback;
  wint_t wide_pushback;
  size_t wide_pushback_bytes;
  bool io_started, input_unbuffered, output_owned;
  bool output_flush_failed; /* Retained queue must drain before accepting more. */
  int output_mode; /* _IONBF by default; configuration precedes stream I/O. */
  unsigned char *output; /* Borrowed caller storage, or owned allocation. */
  size_t output_capacity, output_count;
  bool memory_stream;
  char *memory; /* Stream-owned until close; publication borrows this allocation. */
  size_t memory_capacity, memory_length, memory_position;
  char **memory_pointer;
  size_t *memory_size;
};

int stream_error(FILE *stream, int error);
bool stream_ready(FILE *stream, bool writing);
bool stream_byte_ready(FILE *stream, bool writing);

/* Retention never waits on a FILE lock. Operations retain before locking. */
bool stream_retain(FILE *stream);
/* Descriptor table exclusion guarantees the associated pointer is alive. */
bool stream_retain_association(FILE *stream);
void stream_release(FILE *stream);
bool stream_try_lock(FILE *stream);
void stream_unlock(FILE *stream);
bool stream_acquire(FILE *stream);
void stream_end(FILE *stream);

/* Cleanup of one complete FILE entry operation, including every early return. */
struct stream_operation {
  FILE *stream;
};
void stream_operation_end(struct stream_operation *operation);

#endif
