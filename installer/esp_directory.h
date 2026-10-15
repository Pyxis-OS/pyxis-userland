#ifndef USERSPACE_INSTALLER_ESP_DIRECTORY_H
#define USERSPACE_INSTALLER_ESP_DIRECTORY_H

#include <directory.h>
#include <stdbool.h>

/* Requires ENUMERATE and LOOKUP on the exclusively owned FAT32 parent.
 * Reuses only a canonical name equal under ASCII FAT case rules, never an 8.3
 * alias of another name. CREATE requires parent CREATE; an alias collision
 * refuses without opening it. No retry on a changed directory or failed create.
 * Success owns *child with exactly rights; failure clears it. */
enum call_status install_esp_child_open(handle_t parent, const char *name,
    uint64_t kind, uint64_t rights, bool create, handle_t *child);

#endif
