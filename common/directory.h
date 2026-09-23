#ifndef USERSPACE_COMMON_DIRECTORY_H
#define USERSPACE_COMMON_DIRECTORY_H

#include <directory.h>

/* Utility support: owns temporary heap storage; returns one owned handle. */
enum call_status resolve_directory(const char *path, uint64_t rights, handle_t *directory);
int report_directory_error(const char *program, const char *path, enum call_status status);

#endif
