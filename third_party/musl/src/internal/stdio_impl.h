#ifndef PYXIS_MUSL_STDIO_IMPL_H
#define PYXIS_MUSL_STDIO_IMPL_H

#include <stdio.h>
#include <stddef.h>

/* Pyxis: stands in for musl's private stream header, which getopt.c and
 * getopt_long.c include. Libc's FILE synchronizes itself, so musl's stream
 * lock has nothing to lock. */
#define FLOCK(f)
#define FUNLOCK(f)

void __getopt_msg(const char *, const char *, const char *, size_t);

#endif
