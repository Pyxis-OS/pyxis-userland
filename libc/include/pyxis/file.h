#ifndef LIBC_PYXIS_FILE_H
#define LIBC_PYXIS_FILE_H

#include <stdint.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* open() flags and creation mode, with writable admission restricted to native
 * FILE routes. Actual exported/provider/transaction targets fail with ENOTSUP
 * before OPEN, STAGE, creation or truncation. Read-only opens retain open()'s
 * routes. Creation accepts only 0666; mode is ignored without O_CREAT.
 * O_APPEND uses native APPEND, atomic per operation of at most 4,080 bytes;
 * larger writes may interleave. Unsupported backings refuse before effects. */
int pyxis_file_open(const char *path, int flags, mode_t mode);

#define PYXIS_COPY_UNEXPECTED_EOF 1

/* Copy the initial regular FILE length through retained READ/WRITE objects.
 * Existing native destinations retain their identity; known aliases fail with
 * EINVAL and unprovable distinction with ENOTSUP before resize. Missing names
 * use exclusive CREATE, never reopen a concurrent creator. Provider READ FILEs
 * are allowed; destination providers/exports are refused before writer OPEN.
 * Copy only bytes and length, using one cwd snapshot for relative operands and
 * one capture per explicit root. No metadata copy, sync, rollback or pathname
 * cleanup: an empty/partial destination may remain on failure.
 * Return 0 and set copied only after checked closes; -1 sets errno. Premature
 * source EOF returns PYXIS_COPY_UNEXPECTED_EOF with EIO instead. Both failure
 * returns preserve copied and the first error through remaining cleanup. */
int pyxis_file_copy(const char *source, const char *destination, uint64_t *copied);

/* Sample metadata and actual grant rights from one admitted FILE, PIPE or CONSOLE descriptor,
 * retaining its open object and operation exclusion throughout both queries.
 * Does not change the cursor or upgrade the grant. Unreported response-stream metadata fails
 * with ENOTSUP. Failure sets errno and preserves both outputs. */
int pyxis_file_metadata(int descriptor, struct stat *metadata, uint64_t *rights);

/* Capture one cwd chain or environment value, then sample one retained object.
 * Native FILE lookup obtains every READ/WRITE right delegated by its parent;
 * DIRECTORY lookup retains its delegated rights. FILE WRITE and DIRECTORY
 * CREATE or REMOVE describe different kinds of mutation authority. No create,
 * truncate, permission mutation or writable provider OPEN occurs here.
 * Provider paths with OPEN_WRITE fail with ENOTSUP before OPEN: writability
 * cannot be learned without preparing a writer. Other provider paths use READ
 * and report the actual received FILE rights. Optional stat validity is kept.
 * Failure sets errno and preserves both outputs. No reference survives return. */
int pyxis_path_metadata(const char *path, struct stat *metadata, uint64_t *rights);

/* Metadata from one captured native FILE or DIRECTORY. Provider/exported
 * routes fail before OPEN, so status and traversal cannot trigger a request. */
int pyxis_path_metadata_native(const char *path, struct stat *metadata, uint64_t *rights);

#ifdef __cplusplus
}
#endif

#endif
