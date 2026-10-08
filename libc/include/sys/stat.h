#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File-type bits of st_mode. Native objects have no permission bits. */
#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)

/* Only what native objects report: a type and, for files, a size. Pyxis has no
 * permission bits, owners, link counts, device/inode identity or timestamps,
 * so there are no fields for them; code that reads one fails to compile. */
struct stat {
  mode_t st_mode;
  off_t st_size; /* Zero for anything but a file. */
};

/* Resolve path like fopen. A file is opened with READ, or WRITE if READ is
 * denied, to learn its size; a file with neither right fails with EACCES. A
 * directory needs only LOOKUP on the way to it. Lookup never follows a
 * symlink, so a symlink entry fails with the lookup's error (ENOTSUP on a host
 * directory). A provider URI is opened as a file, which for HTTP(S) performs
 * the request. Sizes above the off_t range fail with EOVERFLOW. */
int stat(const char *__restrict path, struct stat *__restrict result);
/* Files report S_IFREG and their size, consoles S_IFCHR and pipes S_IFIFO
 * (both size zero). */
int fstat(int descriptor, struct stat *result);

/* Create one directory, resolving path like fopen: startup roots and the
 * initial working-directory chain. Requires CREATE on the parent. mode has no
 * effect; native directories carry no permission bits. Missing parents are not
 * created. Returns 0, or -1 with errno (EEXIST if the name exists). */
int mkdir(const char *path, mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
