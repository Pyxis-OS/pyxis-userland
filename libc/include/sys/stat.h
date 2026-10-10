#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include <sys/types.h>
#include <time.h>

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

#define STAT_DEV_VALID (UINT64_C(1) << 0)
#define STAT_INO_VALID (UINT64_C(1) << 1)
#define STAT_MTIME_VALID (UINT64_C(1) << 2)

/* st_valid covers optional metadata only; type and file size are always given.
 * Check each bit before using its field: zero is not an absence marker. st_dev
 * is the native identity domain, st_ino its object ID, not host device encoding.
 * Different valid domains prove distinctness without inode IDs; full identity
 * needs both bits. Retain references while comparing: tokens have no promise
 * after final release or reboot. mtime may repeat or move backwards; it is not
 * a change count. No permission bits, owners or link counts are invented. */
struct stat {
  mode_t st_mode;
  off_t st_size; /* Zero for anything but a file. */
  dev_t st_dev;
  ino_t st_ino;
  struct timespec st_mtim;
  uint64_t st_valid;
};

/* Resolve path like fopen. A file is opened with READ, or WRITE if READ is
 * denied, to learn its size; a file with neither right fails with EACCES. A
 * directory needs only LOOKUP on the way to it. Lookup never follows a
 * symlink, so a symlink entry fails with the lookup's error (ENOTSUP on a host
 * directory). A provider URI is opened as a file, which for HTTP(S) performs
 * the request. Sizes above the off_t range fail with EOVERFLOW. Unsupported
 * metadata leaves its validity bits clear without hiding other query errors.
 * Path stat does not retain a reference after return; use open/fstat when
 * identity comparison needs to keep the underlying object alive. */
int stat(const char *__restrict path, struct stat *__restrict result);
/* Equivalent to stat only under the current no-follow lookup contract: native
 * filesystems have no symlinks and host symlink lookup fails with ENOTSUP.
 * Revisit this implementation if symlink metadata or following is introduced. */
int lstat(const char *__restrict path, struct stat *__restrict result);
/* Files report S_IFREG, size and available optional metadata. Consoles report
 * S_IFCHR and pipes S_IFIFO, both with size zero and optional fields invalid. */
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
