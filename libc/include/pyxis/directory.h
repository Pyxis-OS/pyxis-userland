#ifndef LIBC_PYXIS_DIRECTORY_H
#define LIBC_PYXIS_DIRECTORY_H

#include <dirent.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native-only opendir retaining the resolved directory's actual rights. Only
 * ENUMERATE is required on the result; traversing parents needs LOOKUP. Provider
 * and exported DIRECTORY routes fail before invocation. readdir/closedir own
 * the usual cursor, entry buffer and lifetime. */
DIR *pyxis_directory_open(const char *path);

/* Borrow a live DIR and resolve exactly one ordinary component through its
 * retained parent, never cwd, environment or a reconstructed path. The caller
 * must retain the owner through each call, serialize readdir calls, and prevent
 * closedir from racing any call. These helpers read only the immutable handle,
 * so may run concurrently with each other and readdir. They never advance the
 * cursor or retain the child.
 * Removal/replacement may change results since enumeration. No link following.
 *
 * Metadata requests the FILE READ/WRITE obtainable from held parent rights, or
 * the parent's actual DIRECTORY rights, then samples the actual child and its
 * granted rights. A FILE without READ or WRITE cannot report metadata.
 * Type uses a zero-right typed lookup (requiring only parent LOOKUP), returning
 * DT_REG or DT_DIR. Known enumerated d_type values need no such lookup.
 * Both outputs remain unchanged on any failure, including child close failure. */
int pyxis_directory_entry_metadata(DIR *directory, const char *component,
    struct stat *metadata, uint64_t *rights);
int pyxis_directory_entry_type(DIR *directory, const char *component, unsigned char *type);

/* Native path mutations, without provider OPEN/REMOVE. Resolve original path
 * components from one captured cwd/root, reject leading / and boundary escape,
 * preserve native rights, backend type/store restrictions and failure results.
 * No symlink following, automatic mutation retry, copy fallback or implicit
 * sync. Failure may follow effects; no rollback or durability is promised. */
int pyxis_directory_create(const char *path);
int pyxis_directory_unlink(const char *path);
int pyxis_directory_remove(const char *path);
/* Remove the named file or empty directory through the parent's REMOVE right,
 * without first looking up or opening the target. */
int pyxis_directory_remove_any(const char *path);
int pyxis_directory_rename(const char *source, const char *destination);

/* Empty input succeeds without lookup. Otherwise create/check each original
 * component in order, retaining the authorized chain. Existing directories,
 * including roots/final . or .., undergo real fallible checks; concurrent
 * creators are checked once. Failure can leave a completed creation prefix. */
int pyxis_directory_create_all(const char *path);

/* Report the exact number of directories created in the same ordered walk.
 * Output is unchanged on failure even if an earlier component was created. */
int pyxis_directory_create_all_count(const char *path, uintmax_t *created);

/* Remove a native directory tree or a symlink operand, without following links.
 * Regular-file operands fail with ENOTDIR; other special entries are unsupported.
 * Roots and final . or .. are invalid, and a trailing / requires a directory.
 * Capture cwd once for relative operands and each explicit root once; descendants
 * use retained parents, never reconstructed paths. No depth/name policy limit:
 * fallible heap/handle/backend limits apply. Provider/exported routes refuse.
 * Enumeration is live: restart after confirmed removal, allowing eight observed
 * CHANGED restarts without removal before EAGAIN. Initial absence is ENOENT;
 * disappearance after admission is tolerated. Other errors stop work, preserving
 * partial effects; all owned closes are checked without hiding the primary error.
 * No rollback, mutation retry or sync. Concurrent moves can leave a held tree
 * being emptied elsewhere; final removal selects its original name. There is
 * no cwd-ancestor or host mount guard, and final NOT_EMPTY/BUSY is returned. */
int pyxis_directory_remove_all(const char *path);

/* The same one-pass retained tree removal with an exact successful-removal
 * count. The output is unchanged on failure; effects may still be partial. */
int pyxis_directory_remove_all_count(const char *path, uintmax_t *removed);

#ifdef __cplusplus
}
#endif

#endif
