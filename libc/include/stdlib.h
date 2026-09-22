#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Single-threaded allocator; allocations are aligned to at least 16 bytes.
 * Zero-size allocations return NULL. realloc(p, 0) frees p and returns NULL.
 * Failure sets errno to ENOMEM and leaves an existing allocation intact.
 * Pools remain mapped until process exit. */
void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);

/* Borrowed immutable startup value; do not modify or free the result. */
char *getenv(const char *name);

/* No stdio streams or exit handlers exist yet; both terminate immediately. */
[[noreturn]] void exit(int status);
[[noreturn]] void _Exit(int status);

#endif
