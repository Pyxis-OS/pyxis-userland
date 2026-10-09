#ifndef USERSPACE_MACHINE_SETTINGS_H
#define USERSPACE_MACHINE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define MACHINE_HOSTNAME_MAX 63
#define MACHINE_HOSTNAME_DEFAULT "pyxis"
#define MACHINE_HOSTNAME_PATH "config/machine/network/hostname"

/* One ASCII label, preserved as written. LENGTH excludes any file newline
 * and NUL terminator; numeric labels are valid. No locale or ABI dependency. */
bool machine_hostname_valid(const char *name, size_t length);

#endif
