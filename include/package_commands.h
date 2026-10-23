#ifndef USERSPACE_PACKAGE_COMMANDS_H
#define USERSPACE_PACKAGE_COMMANDS_H

#include <abi/package_commands.h>
#include <abi/syscall.h>

/* All inputs are borrowed native DIRECTORY grants with zero transport.
 * Success returns a new owned read-only DIRECTORY; failure clears output. */
enum call_status package_commands_open(handle_t volume, handle_t *live);
/* A live read-only directory of the active set's package trees: looking up a
 * package ID returns its tree, read at that moment. */
enum call_status package_roots_open(handle_t volume, handle_t *live);
/* A fixed directory of the trees of the packages selected so far from the
 * fixed capture FIXED: PREVIOUS's selection, or none when it is INVALID, plus
 * ID. The result has the rights of both and never more than read-only. */
enum call_status package_roots_select(handle_t fixed, const char *id, handle_t previous,
    handle_t *selection);
/* Live views report set/count zero. Fixed views report captured provenance. */
enum call_status package_commands_info(handle_t view, struct package_command_info *info);
/* previous is zero for a fresh capture, or a held fixed view from the same
 * origin to reuse regardless of a later current switch. Both lineages apply.
 * A fixed source refuses a previous view of a different set. */
enum call_status package_commands_capture(handle_t view, handle_t previous, handle_t *fixed);
/* Freeze an independently held volume root for this fixed selection. The
 * kernel checks actual volume identity and preserves both CUT lineages; the
 * result has only the source root's held read-only rights. A command view alone
 * cannot obtain the backing root. Retargeting SOURCE does not retarget ROOT. */
enum call_status package_commands_bind(handle_t source, handle_t fixed, handle_t *root);
/* Resolve one name from a fixed view. Only read-only FILE/DIRECTORY rights are
 * allowed. Success owns reply->handle and includes the immutable target record;
 * failure closes provisional grants and clears the reply. */
enum call_status package_commands_lookup(handle_t fixed, const char *name,
    uint64_t file_rights, uint64_t directory_rights,
    struct package_command_lookup_reply *reply);

#endif
