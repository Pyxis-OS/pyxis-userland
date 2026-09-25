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

/* Exclusive creation of an empty regular file/directory. Requires CREATE; child
 * rights are bounded exactly as for lookup. An existing name returns
 * ALREADY_EXISTS, without opening/replacing it. Initrd rejects mutation even
 * when a grant carries CREATE. Success owns *handle; failure clears it. Host
 * failure may follow a change. May wait for allocation, table growth or I/O. */
enum call_status directory_create(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle);

/* Remove a single name with REMOVE on its parent. kind is FILE, DIRECTORY or
 * ANY; nonempty directories fail with NOT_EMPTY. Existing handles survive.
 * Removed RAM directories reject subsequent creation with NOT_FOUND. Host
 * name/type preflight can race an external change; failure may have effects.
 * Host links are never followed as the requested file/directory kind. */
enum call_status directory_remove(handle_t directory, const char *name, uint64_t kind);

/* Atomic file-only move/replacement. Source needs REMOVE; destination needs
 * CREATE. Host REPLACE always needs destination REMOVE; RAM needs it when
 * replacing a different entry. policy is DIRECTORY_RENAME_NO_REPLACE or
 * DIRECTORY_RENAME_REPLACE. Both names are single components. Existing handles
 * survive. Host name/type preflight can race external changes, and failure may
 * have effects. Host links are not followed as regular files. */
enum call_status directory_rename(handle_t source, const char *source_name,
    handle_t destination, const char *destination_name, uint64_t policy);

/* Requires CREATE or REMOVE on this directory handle. Flushes its metadata
 * and namespace changes; RAM backing succeeds without work. A submitted call
 * with an untrustworthy reply reports CALL_OUTCOME_UNKNOWN, so do not retry it
 * automatically. */
enum call_status directory_sync(handle_t directory);

/* Pass a zero cursor initially, then the returned cursor for the same directory.
 * On CALL_OK inspect reply->outcome: ENTRY, END, BUFFER_TOO_SMALL or CHANGED.
 * Only ENTRY writes name (including NUL). BUFFER_TOO_SMALL reports name_size
 * and leaves the cursor unchanged. CHANGED requires restarting explicitly.
 * Enumeration is live, not a snapshot; external changes may go undetected.
 * Treat both cursor fields as opaque, not as entry counts.
 * reply and the name buffer must be disjoint. Input cursor may alias reply's
 * cursor; it is captured before reply is cleared. Errors clear *reply. */
enum call_status directory_enumerate(handle_t directory, const struct directory_cursor *cursor,
    char *name, size_t capacity, struct directory_enumerate_reply *reply);

#endif
