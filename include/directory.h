#ifndef USERSPACE_DIRECTORY_H
#define USERSPACE_DIRECTORY_H

#include <abi/directory.h>
#include <abi/syscall.h>

/* Returns native status. Success gives a new owned handle; close it when done.
 * name is one NUL-terminated component. Rights apply to the requested kind;
 * directory grants are bounded by the parent, file READ by READ_FILES.
 * *handle is cleared on failure. No allocation or path parsing. */
enum call_status directory_lookup(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle);

/* Exclusive creation of an empty RAM file/directory. Requires CREATE; child
 * rights are bounded exactly as for lookup. An existing name returns
 * ALREADY_EXISTS, without opening/replacing it. Initrd rejects mutation even
 * when a grant carries CREATE. Success owns *handle; failure clears it and
 * publishes no entry. May wait for BSP allocation, table growth or disposal. */
enum call_status directory_create(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle);

/* Remove a single name with REMOVE on its parent. kind is FILE, DIRECTORY or
 * ANY; nonempty directories fail with NOT_EMPTY. Existing handles survive.
 * Removed directories reject subsequent creation with NOT_FOUND. */
enum call_status directory_remove(handle_t directory, const char *name, uint64_t kind);

/* Atomic file-only move/replacement. Source needs REMOVE; destination needs
 * CREATE and, only when replacing a different entry, REMOVE. policy is
 * DIRECTORY_RENAME_NO_REPLACE or DIRECTORY_RENAME_REPLACE. Both names are
 * single components. Existing handles survive; failure preserves both names. */
enum call_status directory_rename(handle_t source, const char *source_name,
    handle_t destination, const char *destination_name, uint64_t policy);

/* Pass a zero cursor initially, then the returned cursor for the same directory.
 * On CALL_OK inspect reply->outcome: ENTRY, END, BUFFER_TOO_SMALL or CHANGED.
 * Only ENTRY writes name (including NUL). BUFFER_TOO_SMALL reports name_size
 * and leaves the cursor unchanged. CHANGED requires restarting explicitly.
 * reply and the name buffer must be disjoint. Input cursor may alias reply's
 * cursor; it is captured before reply is cleared. Errors clear *reply. */
enum call_status directory_enumerate(handle_t directory, const struct directory_cursor *cursor,
    char *name, size_t capacity, struct directory_enumerate_reply *reply);

#endif
