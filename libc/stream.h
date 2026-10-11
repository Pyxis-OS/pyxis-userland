#ifndef LIBC_STREAM_H
#define LIBC_STREAM_H

#include <stdio.h>
#include <wchar.h>

enum stream_orientation {
  STREAM_UNORIENTED = 0, STREAM_BYTE_ORIENTED = -1, STREAM_WIDE_ORIENTED = 1,
};

struct pyxis_file {
  struct pyxis_file *next;
  int descriptor; /* Non-owning association; descriptor close sets this to -1. */
  bool readable, writable; /* FILE selection can narrow descriptor access. */
  bool eof, error, closed, allocated;
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

};

int stream_error(FILE *stream, int error);
bool stream_ready(FILE *stream, bool writing);

#endif
