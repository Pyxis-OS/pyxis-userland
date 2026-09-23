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

/* ASCII whitespace/sign and bases 2..36; base 0 detects decimal, octal, 0x or
 * C23 0b prefixes. No digits returns zero and leaves *end at text. Overflow
 * saturates with ERANGE but still consumes all valid digits. Invalid bases
 * return zero with EINVAL and *end at text. Otherwise errno is unchanged.
 * end may be NULL. Unsigned conversions apply a minus sign modulo their range. */
long strtol(const char *restrict text, char **restrict end, int base);
unsigned long strtoul(const char *restrict text, char **restrict end, int base);
long long strtoll(const char *restrict text, char **restrict end, int base);
unsigned long long strtoull(const char *restrict text, char **restrict end, int base);
/* Decimal conversion without end/range diagnostics; use strtol for those.
 * As in C, an out-of-int-range input has no guaranteed result. */
int atoi(const char *text);

/* No stdio streams or exit handlers exist yet; both terminate immediately. */
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);

#endif
