#ifndef LIBC_DIRENT_H
#define LIBC_DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

/* d_type values. Native "other" and "unknown" entries report DT_UNKNOWN. */
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10

struct dirent {
  unsigned char d_type;
  char d_name[]; /* NUL-terminated; native names have no fixed maximum. */
};

typedef struct libc_directory DIR;

/* Resolve path like fopen and open the directory with ENUMERATE. Provider URIs
 * are not directories and fail with ENODEV. */
DIR *opendir(const char *path);
/* Native enumeration is live, not a snapshot, and has no . or .. entries. The
 * entry belongs to the DIR and stays valid until the next readdir or closedir.
 * End of directory returns NULL with errno unchanged. A detected concurrent
 * change also returns NULL, with EAGAIN; the listing so far may be incomplete. */
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);

#ifdef __cplusplus
}
#endif

#endif
