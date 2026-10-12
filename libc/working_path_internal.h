#ifndef LIBC_WORKING_PATH_INTERNAL_H
#define LIBC_WORKING_PATH_INTERNAL_H

#include <abi/syscall.h>
#include <pyxis/working_path.h>

/* Description only: callers must separately resolve the original input. */
enum call_status libc_path_description(const char *current, const char *path, char **result);

/* Clone and optionally resolve from one retained caller-owned snapshot. */
enum call_status libc_working_snapshot_from(const struct pyxis_working_snapshot *source,
    struct pyxis_working_snapshot *snapshot, const char *cwd_override);

#endif
