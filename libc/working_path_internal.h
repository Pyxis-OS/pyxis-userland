#ifndef LIBC_WORKING_PATH_INTERNAL_H
#define LIBC_WORKING_PATH_INTERNAL_H

#include <abi/syscall.h>

/* Description only: callers must separately resolve the original input. */
enum call_status libc_path_description(const char *current, const char *path, char **result);

#endif
