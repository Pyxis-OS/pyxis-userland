#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct pyxis_file FILE;

#define EOF (-1)
#define BUFSIZ 8192
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* All streams are unbuffered. fopen accepts r/w/a, optional + and optional b
 * (no effect), resolving native capability paths and the initial directory.
 * Private descriptors own handles and file offsets; FILE owns its association
 * and indicators. Standard descriptors adopt exclusive startup handles once.
 * No fdopen/fileno, setvbuf, freopen, pushback, scanning or wide I/O here. */
FILE *fopen(const char *restrict path, const char *restrict mode);
int fclose(FILE *stream);
/* Remove a file or empty directory through its parent capability. Existing
 * streams keep their object; roots and final . or .. are not removable. */
int remove(const char *path);
/* File-only atomic rename/replacement. Paths use explicit startup roots/cwd;
 * directories and cross-filesystem copy fallbacks are not supported. */
int rename(const char *old_path, const char *new_path);
int fflush(FILE *stream);
size_t fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream);
/* Wait for initial data/EOF/error, returning up to capacity bytes without
 * filling a short result. Only backend EOF sets the EOF indicator; unavailable
 * input is EBADF and independent terminal input also reports EOF. Existing EOF
 * suppresses reads until cleared. Zero capacity changes no indicators. */
size_t fread_some(void *restrict buffer, size_t capacity, FILE *restrict stream);
size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *fgets(char *restrict buffer, int capacity, FILE *restrict stream);
/* Read through the next newline, or to EOF for a final unterminated line, into
 * *line, growing it with realloc and updating *capacity. A NULL *line starts
 * with zero capacity. Returns the byte count including any newline and
 * excluding the added NUL; embedded NUL bytes are ordinary input. EOF before
 * any byte returns -1. Null arguments (EINVAL), read errors, allocation
 * failure (ENOMEM) and a line longer than SSIZE_MAX (EOVERFLOW) return -1, set
 * errno and the error indicator, and leave *line and *capacity describing the
 * caller's current allocation. Reads one byte per call to the unbuffered
 * stream, so each byte is a separate native read. */
ssize_t getline(char **restrict line, size_t *restrict capacity, FILE *restrict stream);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
int fputs(const char *restrict text, FILE *restrict stream);
int puts(const char *text);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fprintf(FILE *restrict stream, const char *restrict format, ...)
  __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *restrict stream, const char *restrict format, va_list args)
  __attribute__((format(printf, 2, 0)));
int printf(const char *restrict format, ...)
  __attribute__((format(printf, 1, 2)));
int vprintf(const char *restrict format, va_list args)
  __attribute__((format(printf, 1, 0)));
/* Writes to stderr and preserves errno, even if reporting fails. */
void perror(const char *prefix);

/* All printf-family functions support s, c, d, i, u, o, x, X, p and %;
 * integer lengths hh/h/l/ll/j/z/t, flags -+ #0, width and precision (also *).
 * Floating conversions f/F/e/E/g/G/a/A accept double (also l) or long double
 * (L), including infinities, NaNs and signed zero. The radix is always a dot.
 * Unsupported formats fail with EINVAL; a count above INT_MAX uses EOVERFLOW.
 * snprintf/vsnprintf return the full length excluding NUL even when truncated;
 * their destination may be NULL for zero capacity. Stream formatting returns
 * bytes written, or a negative result on formatting/allocation/output failure. */
int snprintf(char *restrict buffer, size_t capacity, const char *restrict format, ...)
  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *restrict buffer, size_t capacity, const char *restrict format, va_list args)
  __attribute__((format(printf, 3, 0)));

/* Allocate a NUL-terminated result, including for an empty string. Success
 * transfers ownership to the caller and returns the length excluding NUL.
 * Failure returns -1, sets errno and leaves *output NULL. Uses the same formats
 * and INT_MAX count limit as snprintf; vasprintf preserves its argument list. */
int asprintf(char **restrict output, const char *restrict format, ...)
  __attribute__((format(printf, 2, 3)));
int vasprintf(char **restrict output, const char *restrict format, va_list args)
  __attribute__((format(printf, 2, 0)));

#endif
