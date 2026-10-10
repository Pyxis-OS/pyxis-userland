#ifndef LIBC_STREAM_H
#define LIBC_STREAM_H

#include <stdio.h>

struct pyxis_file {
  struct pyxis_file *next;
  int descriptor; /* Non-owning association; descriptor close sets this to -1. */
  bool readable, writable; /* FILE selection can narrow descriptor access. */
  bool eof, error, closed, allocated;
  bool has_pushback; /* One ungetc byte, returned before any descriptor input. */
  unsigned char pushback;
  bool io_started, input_unbuffered, output_owned;
  bool output_flush_failed; /* Retained queue must drain before accepting more. */
  int output_mode; /* _IONBF by default; configuration precedes stream I/O. */
  unsigned char *output; /* Borrowed caller storage, or owned allocation. */
  size_t output_capacity, output_count;

};

int stream_error(FILE *stream, int error);
bool stream_ready(FILE *stream, bool writing);

#endif
