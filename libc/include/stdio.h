#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct pyxis_file FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* All streams are unbuffered. fopen accepts r/w/a, optional + and optional b
 * (no effect), resolving native capability paths and the initial directory.
 * FILE owns its handle and independent offset. Standard streams own copies of
 * the startup console grants; closing one cannot invalidate the others.
 * No fd API, setvbuf, freopen, pushback, scanning or wide I/O in this slice. */
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
size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *fgets(char *restrict buffer, int capacity, FILE *restrict stream);
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
 * Unsupported formats fail with EINVAL; a count above INT_MAX uses EOVERFLOW.
 * snprintf/vsnprintf return the full length excluding NUL even when truncated;
 * their destination may be NULL for zero capacity. Stream formatting returns
 * bytes written, or a negative result on formatting/allocation/output failure. */
int snprintf(char *restrict buffer, size_t capacity, const char *restrict format, ...)
  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *restrict buffer, size_t capacity, const char *restrict format, va_list args)
  __attribute__((format(printf, 3, 0)));

#endif
