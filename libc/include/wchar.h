#ifndef LIBC_WCHAR_H
#define LIBC_WCHAR_H

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

typedef unsigned int wint_t;
#define WEOF ((wint_t)-1)

/* All-zero initialization is the initial UTF-8 state. An incomplete scalar
 * stores its accumulated value, remaining continuation bytes and total length.
 * Conversion errors reset the state. Implicit states are single-threaded. */
typedef struct {
  unsigned int value;
  unsigned int remaining;
  unsigned int length;
} mbstate_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Count wide characters before the terminating null. */
size_t wcslen(const wchar_t *string);

/* Native C-locale numeric grammar and range/error semantics. */
long wcstol(const wchar_t *__restrict text, wchar_t **__restrict end, int base);
unsigned long wcstoul(const wchar_t *__restrict text, wchar_t **__restrict end, int base);
long long wcstoll(const wchar_t *__restrict text, wchar_t **__restrict end, int base);
unsigned long long wcstoull(const wchar_t *__restrict text, wchar_t **__restrict end, int base);
float wcstof(const wchar_t *__restrict text, wchar_t **__restrict end);
double wcstod(const wchar_t *__restrict text, wchar_t **__restrict end);
long double wcstold(const wchar_t *__restrict text, wchar_t **__restrict end);
/* Counts wide characters; insufficient capacity returns -1, never truncated
 * success. Native numeric formats, Unicode literals and c/s with optional l;
 * unsupported conversions (including n) fail with EINVAL. */
int swprintf(wchar_t *__restrict output, size_t capacity,
    const wchar_t *__restrict format, ...);
int vswprintf(wchar_t *__restrict output, size_t capacity,
    const wchar_t *__restrict format, va_list args);

/* An unoriented stream becomes wide; byte-oriented streams fail with EINVAL.
 * UTF-8 conversion and one complete character of pushback belong to the FILE,
 * independently of descriptor read-ahead. Failed seek preserves that state. */
int fwide(FILE *stream, int mode);
wint_t fgetwc(FILE *stream);
wint_t getwc(FILE *stream);
wint_t ungetwc(wint_t character, FILE *stream);
wint_t fputwc(wchar_t character, FILE *stream);
wint_t putwc(wchar_t character, FILE *stream);

int mbsinit(const mbstate_t *state);
/* Return bytes consumed from this call, zero for NUL, (size_t)-2 for an
 * incomplete scalar, or (size_t)-1 with EILSEQ. NULL text converts a NUL.
 * A NULL state selects the function's own persistent conversion state. */
size_t mbrtowc(wchar_t *__restrict output, const char *__restrict text,
  size_t size, mbstate_t *__restrict state);
size_t mbrlen(const char *__restrict text, size_t size,
  mbstate_t *__restrict state);
/* Encode one Unicode scalar; NULL output resets state and returns one.
 * Invalid scalars return (size_t)-1 with EILSEQ without writing output. */
size_t wcrtomb(char *__restrict output, wchar_t value,
  mbstate_t *__restrict state);
/* Only ASCII is a complete single-byte UTF-8 character. */
wint_t btowc(int byte);
int wctob(wint_t value);

/* Counts exclude NUL. With output, *source advances past consumed input and
 * becomes NULL after NUL. An error leaves it at the failing conversion's input
 * in the current call, even if the prefix was consumed by an earlier call.
 * Bounded decoding saves incomplete input in state and consumes those bytes.
 * Output exhaustion never consumes a scalar. NULL output counts without
 * changing *source or state, and ignores the output capacity. */
size_t mbsrtowcs(wchar_t *__restrict output, const char **__restrict source,
  size_t capacity, mbstate_t *__restrict state);
size_t wcsrtombs(char *__restrict output, const wchar_t **__restrict source,
  size_t capacity, mbstate_t *__restrict state);
size_t mbsnrtowcs(wchar_t *__restrict output, const char **__restrict source,
  size_t bytes, size_t capacity, mbstate_t *__restrict state);
size_t wcsnrtombs(char *__restrict output, const wchar_t **__restrict source,
  size_t count, size_t capacity, mbstate_t *__restrict state);

wchar_t *wmemcpy(wchar_t *__restrict output, const wchar_t *__restrict input,
  size_t count);
wchar_t *wmemmove(wchar_t *output, const wchar_t *input, size_t count);
wchar_t *wmemset(wchar_t *output, wchar_t value, size_t count);
wchar_t *wmemchr(const wchar_t *input, wchar_t value, size_t count);
int wmemcmp(const wchar_t *left, const wchar_t *right, size_t count);
int wcscmp(const wchar_t *left, const wchar_t *right);
int wcsncmp(const wchar_t *left, const wchar_t *right, size_t count);
/* C-locale collation is wide-character identity order. */
int wcscoll(const wchar_t *left, const wchar_t *right);
size_t wcsxfrm(wchar_t *__restrict output, const wchar_t *__restrict input,
  size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
