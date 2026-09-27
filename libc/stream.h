#ifndef LIBC_STREAM_H
#define LIBC_STREAM_H

#include <stdio.h>

struct pyxis_file {
  struct pyxis_file *next;
  int descriptor; /* Non-owning association; descriptor close sets this to -1. */
  bool eof, error, closed, allocated;
};

int stream_error(FILE *stream, int error);
bool stream_ready(FILE *stream, bool writing);

#endif
