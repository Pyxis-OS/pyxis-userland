#ifndef USERSPACE_MACHINE_SETTINGS_H
#define USERSPACE_MACHINE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define MACHINE_HOSTNAME_MAX 63
#define MACHINE_HOSTNAME_DEFAULT "pyxis"
#define MACHINE_HOSTNAME_PATH "config/machine/network/hostname"
#define MACHINE_SCROLL_DIRECTION_MAX 11
#define MACHINE_MOUSE_SCROLL_PATH "config/machine/input/mouse/scroll-direction"
#define MACHINE_TOUCHPAD_SCROLL_PATH "config/machine/input/touchpad/scroll-direction"

/* One ASCII label, preserved as written. LENGTH excludes any file newline
 * and NUL terminator; numeric labels are valid. No locale or ABI dependency. */
bool machine_hostname_valid(const char *name, size_t length);

/* Exact lowercase natural/traditional, without file newline or NUL. Leaves
 * NATURAL unchanged on failure; the shared schema has no target ABI dependency. */
bool machine_scroll_direction_parse(const char *name, size_t length, bool *natural);

#endif
