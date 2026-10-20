#ifndef USERSPACE_SHELL_SETTINGS_H
#define USERSPACE_SHELL_SETTINGS_H

#include <abi/syscall.h>
#include <handle.h>

#define SHELL_SETTINGS_RESOURCE "shell_settings"
#define SHELL_SETTINGS_PATH "home://.config/shell/startup.lua"

/* Trusted session, pane and Remote startup open the settings file through the
 * home directory they already hold and pass it to the initial interactive
 * shell as a private READ FILE named SHELL_SETTINGS_RESOURCE. CALL_NOT_FOUND
 * means home or a path component is absent; the caller then supplies nothing.
 * The file is data only: the shell validates it and never executes it. */
enum call_status shell_settings_open(handle_t home, handle_t *file);

#endif
