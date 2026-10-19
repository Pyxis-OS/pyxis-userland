#ifndef USERSPACE_PACKAGE_COMMANDS_H
#define USERSPACE_PACKAGE_COMMANDS_H

#include <abi/package_commands.h>
#include <abi/syscall.h>

/* All inputs are borrowed native DIRECTORY grants with zero transport.
 * Success returns a new owned read-only DIRECTORY; failure clears output. */
enum call_status package_commands_open(handle_t volume, handle_t *live);
/* Live views report set/count zero. Fixed views report captured provenance. */
enum call_status package_commands_info(handle_t view, struct package_command_info *info);
/* previous is zero for a fresh capture, or a held fixed view from the same
 * origin to reuse regardless of a later current switch. Both lineages apply.
 * A fixed source refuses a previous view of a different set. */
enum call_status package_commands_capture(handle_t view, handle_t previous, handle_t *fixed);
/* Resolve one name from a fixed view. Only read-only FILE/DIRECTORY rights are
 * allowed. Success owns reply->handle and includes the immutable target record;
 * failure closes provisional grants and clears the reply. */
enum call_status package_commands_lookup(handle_t fixed, const char *name,
    uint64_t file_rights, uint64_t directory_rights,
    struct package_command_lookup_reply *reply);

#endif
