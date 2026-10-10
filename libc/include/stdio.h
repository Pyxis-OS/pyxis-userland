#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pyxis_file FILE;

#define EOF (-1)
#define BUFSIZ 8192
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Output is unbuffered. fread, and the character and line input built on it,
 * may read up to BUFSIZ bytes ahead from files and pipes, never from consoles.
 * fopen accepts r/w/a, optional + and optional b (no effect), resolving native
 * capability paths and the initial directory. Private descriptors own handles,
 * file offsets and read-ahead; FILE owns its association and indicators.
 * Read-ahead is private and never accompanies a delegated stream: do not read a
 * stream you will delegate with buffered input. ftell excludes read-ahead; a
 * successful fseek or a write drops it, a failed seek keeps it. Standard
 * descriptors adopt exclusive startup handles once. No setvbuf, freopen or
 * wide I/O here. */
FILE *fopen(const char *__restrict path, const char *__restrict mode);
/* Associate one FILE with an existing descriptor using fopen mode syntax.
 * Failure retains caller ownership; success transfers it to fclose. Requested
 * access must fit the descriptor, and FILE operations obey that selection.
 * Position/read-ahead are retained; w never creates/truncates. a enables shared
 * non-atomic append policy without moving the cursor. A second association
 * fails with EBUSY; malformed modes use EINVAL, unavailable access EBADF. */
FILE *fdopen(int descriptor, const char *mode);
/* Open an exclusive read/write file under tmp:// and remove its name before
 * return. Requires native random/clock and tmp CREATE/REMOVE/file READ/WRITE.
 * Removal authority is checked before creation. Failure returns NULL with errno;
 * failed or uncertain native creation/removal may leave a named file. */
FILE *tmpfile(void);
int fclose(FILE *stream);
/* Remove a file or empty directory through its parent capability. Existing
 * streams keep their object; roots and final . or .. are not removable. */
int remove(const char *path);
/* File-only atomic rename/replacement. Paths use explicit startup roots/cwd;
 * directories and cross-filesystem copy fallbacks are not supported. */
int rename(const char *old_path, const char *new_path);
int fflush(FILE *stream);
size_t fread(void *__restrict buffer, size_t size, size_t count, FILE *__restrict stream);
/* Return bytes already read ahead, or else wait for initial data/EOF/error
 * from one backend transfer of at most capacity bytes, without filling a short
 * result or reading ahead. Only backend EOF sets the EOF indicator; unavailable
 * input is EBADF and independent terminal input also reports EOF. Existing EOF
 * suppresses reads until cleared. Zero capacity changes no indicators. */
size_t fread_some(void *__restrict buffer, size_t capacity, FILE *__restrict stream);
size_t fwrite(const void *__restrict buffer, size_t size, size_t count, FILE *__restrict stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
char *fgets(char *__restrict buffer, int capacity, FILE *__restrict stream);
/* Read through the next newline, or to EOF for a final unterminated line, into
 * *line, growing it with realloc and updating *capacity. A NULL *line starts
 * with zero capacity. Returns the byte count including any newline and
 * excluding the added NUL; embedded NUL bytes are ordinary input. EOF before
 * any byte returns -1. Null arguments (EINVAL), read errors, allocation
 * failure (ENOMEM) and a line longer than SSIZE_MAX (EOVERFLOW) return -1, set
 * errno and the error indicator, and leave *line and *capacity describing the
 * caller's current allocation. Reads through fgetc, so file and pipe input is
 * fetched in blocks while console input is one native read per byte. */
ssize_t getline(char **__restrict line, size_t *__restrict capacity, FILE *__restrict stream);
/* One byte of pushback per FILE, returned before any further input; a second
 * ungetc before a read fails. It clears EOF and makes ftell one less (but not
 * below zero). A successful fseek, input fflush or any write discards it.
 * Pushback belongs to the FILE, not the descriptor. */
int ungetc(int character, FILE *stream);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
int fputs(const char *__restrict text, FILE *__restrict stream);
int puts(const char *text);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
/* off_t is long here, so these behave exactly like fseek and ftell. */
int fseeko(FILE *stream, off_t offset, int origin);
off_t ftello(FILE *stream);
void rewind(FILE *stream);
/* The descriptor the stream currently uses, or -1 with EBADF for a closed
 * stream or one whose descriptor was closed. The stream keeps ownership:
 * close it with fclose, not close. Pushback and read-ahead belong to the
 * stream and its descriptor, so reading or seeking through the descriptor
 * directly bypasses ungetc and fread's state; fstat is unaffected. */
int fileno(FILE *stream);
/* Directives: whitespace (skips any input whitespace), ordinary characters,
 * %%, and conversions d i u o x X p, a e f g (and capitals), s c [ and n, with
 * * suppression, a nonzero width and lengths hh h l ll j z t L. Scansets accept
 * a leading ^, a leading ] and a-z ranges. Wide conversions (%ls, %lc, %l[) are
 * not supported. Numeric fields stop after 511 characters, as if limited by a
 * width. Each field is read with one character of lookahead; a field that is
 * only a prefix of a number, such as "0x" or "1e+", is consumed and fails.
 * Returns the number of assignments, or EOF if input ended or failed before the
 * first conversion. Malformed or unsupported conversions stop with EINVAL. */
int fscanf(FILE *__restrict stream, const char *__restrict format, ...)
  __attribute__((format(scanf, 2, 3)));
int vfscanf(FILE *__restrict stream, const char *__restrict format, va_list args)
  __attribute__((format(scanf, 2, 0)));
int scanf(const char *__restrict format, ...) __attribute__((format(scanf, 1, 2)));
int vscanf(const char *__restrict format, va_list args) __attribute__((format(scanf, 1, 0)));
int sscanf(const char *__restrict text, const char *__restrict format, ...)
  __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *__restrict text, const char *__restrict format, va_list args)
  __attribute__((format(scanf, 2, 0)));
int fprintf(FILE *__restrict stream, const char *__restrict format, ...)
  __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *__restrict stream, const char *__restrict format, va_list args)
  __attribute__((format(printf, 2, 0)));
int printf(const char *__restrict format, ...)
  __attribute__((format(printf, 1, 2)));
int vprintf(const char *__restrict format, va_list args)
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
int snprintf(char *__restrict buffer, size_t capacity, const char *__restrict format, ...)
  __attribute__((format(printf, 3, 4)));
int vsnprintf(char *__restrict buffer, size_t capacity, const char *__restrict format, va_list args)
  __attribute__((format(printf, 3, 0)));
/* Unbounded forms: buffer must hold the whole result and its NUL. Prefer snprintf. */
int sprintf(char *__restrict buffer, const char *__restrict format, ...)
  __attribute__((format(printf, 2, 3)));
int vsprintf(char *__restrict buffer, const char *__restrict format, va_list args)
  __attribute__((format(printf, 2, 0)));

/* Allocate a NUL-terminated result, including for an empty string. Success
 * transfers ownership to the caller and returns the length excluding NUL.
 * Failure returns -1, sets errno and leaves *output NULL. Uses the same formats
 * and INT_MAX count limit as snprintf; vasprintf preserves its argument list. */
int asprintf(char **__restrict output, const char *__restrict format, ...)
  __attribute__((format(printf, 2, 3)));
int vasprintf(char **__restrict output, const char *__restrict format, va_list args)
  __attribute__((format(printf, 2, 0)));

#ifdef __cplusplus
}
#endif

#endif
