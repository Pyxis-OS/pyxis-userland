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

/* One native lookup selects actual FILE or DIRECTORY, checking only that
 * kind's requested mask against parent authority. Success owns reply->handle;
 * protocol is checked against the new grant. Failure closes provisional grants
 * and clears reply. Exported directory traversal remains unsupported. */
enum call_status directory_lookup_typed(handle_t directory, const char *name,
    uint64_t file_rights, uint64_t directory_rights,
    struct directory_typed_lookup_reply *reply);

/* Exclusive creation of an empty regular file/directory. Requires CREATE; child
 * rights are bounded exactly as for lookup. An existing name returns
 * ALREADY_EXISTS, without opening/replacing it. Initrd rejects mutation even
 * when a grant carries CREATE. Success owns *handle; failure clears it. Host
 * failure may follow a change. May wait for allocation, table growth or I/O. */
enum call_status directory_create(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, handle_t *handle);

/* Creation with before-effects backing support admission. Requirements use
 * DIRECTORY_CREATE_REQUIRE_*; APPEND requires FILE kind plus WRITE rights. */
enum call_status directory_create_required(handle_t directory, const char *name,
    uint64_t kind, uint64_t rights, uint64_t requirements, handle_t *handle);

/* Read-only backing check on a borrowed native directory. Requires CREATE and
 * WRITE_FILES. No name lookup, reservation or mutation; OK does not guarantee
 * later creation/append success. Unsupported backing returns BAD_OPERATION. */
enum call_status directory_append_support(handle_t directory);

/* Derive an owned native npfs directory confined to FILE packages.lua and
 * packages.lua.new. Requires all DIRECTORY_REPOSITORY_VIEW_RIGHTS and grants
 * exactly that subset. No enumeration, child directories or links; rename
 * stays between views of the same retained parent. Failure clears *view. */
enum call_status directory_repository_view(handle_t directory, handle_t *view);

/* Remove a single name with REMOVE on its parent. kind is FILE, DIRECTORY or
 * ANY; nonempty directories fail with NOT_EMPTY. Existing handles survive.
 * Removed RAM directories reject subsequent creation with NOT_FOUND. Host
 * name/type preflight can race an external change; failure may have effects.
 * Host links are never followed as the requested file/directory kind. */
enum call_status directory_remove(handle_t directory, const char *name, uint64_t kind);

/* Atomic file/directory move/replacement within one native store instance.
 * Source needs REMOVE; destination needs CREATE, plus REMOVE for REPLACE. policy is DIRECTORY_RENAME_NO_REPLACE or
 * DIRECTORY_RENAME_REPLACE. kind is ANY, FILE or DIRECTORY and constrains the
 * source entry under the mutation owner; replacement requires the same kind
 * and an empty directory victim. Both names are single components. Existing
 * handles survive. Directory cycles and cross-store moves refuse. Host/FAT32
 * directory moves are unsupported. Host name/type preflight can race external
 * changes, and failure may have effects. Host links are not followed as files. */
enum call_status directory_rename(handle_t source, const char *source_name,
    handle_t destination, const char *destination_name, uint64_t policy, uint64_t kind);

/* Add an exclusive name for one npfs regular file in the same volume. Source
 * requires LOOKUP, READ_FILES and WRITE_FILES; destination requires CREATE.
 * Names are single components. No returned handle, copy fallback or implicit
 * sync; a submitted call with an untrustworthy reply is OUTCOME_UNKNOWN and
 * must not be replayed. Directories and other backends are unsupported. */
enum call_status directory_link(handle_t source, const char *source_name,
    handle_t destination, const char *destination_name);

/* Requires CREATE or REMOVE on this directory handle. Flushes its metadata
 * and namespace changes; RAM backing succeeds without work. A submitted call
 * with an untrustworthy reply reports CALL_OUTCOME_UNKNOWN, so do not retry it
 * automatically. */
enum call_status directory_sync(handle_t directory);

/* Sample this held directory without additional content rights or traversal.
 * Unsupported INFO succeeds with all optional fields unavailable. Other errors
 * clear *info. Identity comparison requires retained references; mtime can
 * repeat or move backwards and never proves unchanged contents. */
enum call_status directory_info(handle_t directory, struct file_info_reply *info);

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

/* Borrowed FILESYSTEM_INFO grant on a native directory. All fields are available
 * on success; failure preserves *info. The 320-byte record describes retained
 * selection, not a new consistency scan. IDs are opaque bytes; generation is
 * the mounted pool's selected journal sequence. pool_allocatable_bytes excludes
 * the two header blocks but includes shared metadata/reserves, so equal pool IDs
 * identify shared capacity, not separate writable allowances. Opening verifies
 * geometry and root envelopes, not global accounting. Used/free bytes,
 * charged bytes, guarantees, quotas and percentages are unavailable, never
 * implied zero. Other directory backends return BAD_OPERATION. */
enum call_status directory_filesystem_info(handle_t directory,
    struct directory_filesystem_info *info);

#endif
