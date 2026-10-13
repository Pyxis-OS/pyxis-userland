#ifndef LIBC_LIBGEN_H
#define LIBC_LIBGEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Textual path splitting; neither call resolves the path or touches a
 * capability. A scheme root such as tmp:// is the root of its path and acts
 * as "/" does in POSIX. A leading "/" is also treated as a root. Repeated and
 * trailing "/" are ignored, and "." and ".." are ordinary names.
 *
 *   path             dirname      basename
 *   tmp://dir/file   tmp://dir    file
 *   tmp://file       tmp://       file
 *   tmp://           tmp://       tmp://
 *   tmp://dir/       tmp://       dir
 *   dir/file         dir          file
 *   file             .            file
 *   dir/             .            dir
 *   NULL or ""       .            .
 *
 * Both accept a modifiable string and may change it: dirname truncates it and
 * basename removes trailing "/". The result points into the argument or to
 * static ".", and is overwritten or invalidated by any later change to the
 * argument. */
char *dirname(char *path);
char *basename(char *path);

#ifdef __cplusplus
}
#endif

#endif
