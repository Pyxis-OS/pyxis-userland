#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767
#define MB_CUR_MAX 4

typedef struct {
  int quot;
  int rem;
} div_t;

typedef struct {
  long quot;
  long rem;
} ldiv_t;

typedef struct {
  long long quot;
  long long rem;
} lldiv_t;

/* Stateless UTF-8 conversion of one Unicode scalar. NULL text resets (returns
 * zero); NUL returns zero and stores L'\0'. Invalid or incomplete input returns
 * -1 with EILSEQ and leaves output unchanged. output may be NULL. */
int mbtowc(wchar_t *__restrict output, const char *__restrict text, size_t size);

/* Single-threaded allocator; allocations are aligned to at least 16 bytes.
 * Zero-size allocations return NULL. realloc(p, 0) frees p and returns NULL.
 * Failure sets errno to ENOMEM and leaves an existing allocation intact.
 * Pools remain mapped until process exit. */
void *malloc(size_t size);
void free(void *pointer);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
/* alignment must be a power of two no larger than 4096; smaller values give
 * the usual 16 bytes. An unsupported alignment returns NULL with EINVAL. The
 * result is released with free; realloc keeps only 16-byte alignment. */
void *aligned_alloc(size_t alignment, size_t size);

/* Single-threaded process environment, copied from immutable startup state.
 * Borrowed values must not be modified or freed; successful mutation may
 * invalidate them. Names are case-sensitive, nonempty and exclude '='.
 * Empty values differ from absence. Invalid input sets EINVAL; allocation
 * failure sets ENOMEM, with no fallback to the startup block. */
char *getenv(const char *name);
/* Copy name/value; overwrite zero preserves an existing value. Failure leaves
 * the environment unchanged. There is no writable environ or putenv alias. */
int setenv(const char *name, const char *value, int overwrite);
/* Removing an absent name succeeds. */
int unsetenv(const char *name);

/* Replace the final six Xs with random filename characters and exclusively
 * create an empty read/write file; the returned descriptor owns its handle.
 * Resolve paths like fopen, using existing directory authority and native
 * creation policy (no Unix permission modes). Native random and clock required.
 * At most 128 candidate attempts; no existing name is opened or truncated.
 * Failure returns -1 with errno and unspecified template contents; uncertain
 * native creation may leave a file. Success preserves errno. */
int mkstemp(char *name_template);

/* The absolute value must be representable (the type's minimum is excluded). */
int abs(int value);
long labs(long value);
long long llabs(long long value);
/* Truncating division; the divisor must be nonzero and the quotient
 * representable. */
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);

/* ASCII whitespace/sign and bases 2..36; base 0 detects decimal, octal, 0x or
 * C23 0b prefixes. No digits returns zero and leaves *end at text. Overflow
 * saturates with ERANGE but still consumes all valid digits. Invalid bases
 * return zero with EINVAL and *end at text. Otherwise errno is unchanged.
 * end may be NULL. Unsigned conversions apply a minus sign modulo their range. */
long strtol(const char *__restrict text, char **__restrict end, int base);
unsigned long strtoul(const char *__restrict text, char **__restrict end, int base);
long long strtoll(const char *__restrict text, char **__restrict end, int base);
unsigned long long strtoull(const char *__restrict text, char **__restrict end, int base);
/* Decimal conversion without end/range diagnostics; use strtol for those.
 * As in C, an out-of-int-range input has no guaranteed result. */
int atoi(const char *text);

/* ASCII decimal/hexadecimal, infinity and NaN; '.' is the decimal separator.
 * If no conversion occurs, return zero with *end at text and errno unchanged.
 * Overflow, nonzero subnormals (including exact ones), and nonzero input rounded
 * to zero set ERANGE. Other conversions preserve errno. end may be NULL.
 * NaN payload text is consumed but does not select a payload or sign. */
float strtof(const char *__restrict text, char **__restrict end);
double strtod(const char *__restrict text, char **__restrict end);
long double strtold(const char *__restrict text, char **__restrict end);
/* strtod without an end pointer. */
double atof(const char *text);

/* Process-wide pseudo-random sequence for games and simple sampling; not for
 * security. The sequence starts as if srand(1) was called. */
int rand(void);
void srand(unsigned seed);

/* Bounded native proof, not physical alias canonicalization. Input and output
 * fit PATH_MAX including NUL. resolved must hold PATH_MAX bytes, or NULL asks
 * for an allocated result freed with free(). Failure leaves resolved unchanged.
 * Providers and unknown/stale ancestry or unavailable live identity fail. */
char *realpath(const char *__restrict path, char *__restrict resolved);

/* In-place, unstable heapsort with no allocation or recursion. compare returns
 * negative, zero or positive for less than, equal to or greater than. */
void qsort(void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *));

/* Binary search of an array already sorted by compare. The key is the first
 * argument to compare and an element the second. Returns a matching element,
 * not necessarily the first of several equal ones, or a null pointer. */
void *bsearch(const void *key, const void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *));

/* Register a handler for exit or a return from main. Handlers run once each,
 * in reverse order of registration, including handlers registered while exit
 * runs them. The first 32 registrations always succeed; later ones use the heap
 * and return nonzero if it is exhausted. Single-threaded, like errno. */
int atexit(void (*handler)(void));
/* exit runs exit handlers, then .fini_array, then closes stdio streams.
 * _Exit and abort skip libc cleanup. */
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
/* No signal delivery: terminate with EXIT_FAILURE, without libc cleanup. */
__attribute__((noreturn)) void abort(void);

#ifdef __cplusplus
}
#endif

#endif
