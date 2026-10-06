#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#define MB_CUR_MAX 4

/* Stateless UTF-8 conversion of one Unicode scalar. NULL text resets (returns
 * zero); NUL returns zero and stores L'\0'. Invalid or incomplete input returns
 * -1 with EILSEQ and leaves output unchanged. output may be NULL. */
int mbtowc(wchar_t *restrict output, const char *restrict text, size_t size);

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

/* The absolute value must be representable as int (INT_MIN is excluded). */
int abs(int value);

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

/* ASCII decimal/hexadecimal, infinity and NaN; '.' is the decimal separator.
 * If no conversion occurs, return zero with *end at text and errno unchanged.
 * Overflow, nonzero subnormals (including exact ones), and nonzero input rounded
 * to zero set ERANGE. Other conversions preserve errno. end may be NULL.
 * NaN payload text is consumed but does not select a payload or sign. */
float strtof(const char *restrict text, char **restrict end);
double strtod(const char *restrict text, char **restrict end);
long double strtold(const char *restrict text, char **restrict end);
/* strtod without an end pointer. */
double atof(const char *text);

/* Process-wide pseudo-random sequence for games and simple sampling; not for
 * security. The sequence starts as if srand(1) was called. */
int rand(void);
void srand(unsigned seed);

/* In-place, unstable heapsort with no allocation or recursion. compare returns
 * negative, zero or positive for less than, equal to or greater than. */
void qsort(void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *));

/* exit closes stdio streams; _Exit skips libc cleanup. No exit handlers yet. */
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
/* No signal delivery: terminate with EXIT_FAILURE, without libc cleanup. */
__attribute__((noreturn)) void abort(void);

#endif
