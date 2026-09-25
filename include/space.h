#ifndef USERSPACE_SPACE_H
#define USERSPACE_SPACE_H

#include <abi/handle.h>
#include <abi/space.h>
#include <abi/syscall.h>

/* SET_TITLE authority for the caller's space. Nonempty printable ASCII, at
 * most SPACE_TITLE_MAX bytes. Copies the title; caller retains the string. */
enum call_status space_set_title(handle_t space, const char *title);

#endif
