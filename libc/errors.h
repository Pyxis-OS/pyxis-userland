#ifndef LIBC_ERRORS_H
#define LIBC_ERRORS_H

#include <abi/syscall.h>

int libc_call_errno(enum call_status status);
/* As libc_call_errno for a call that resolved path (second may be NULL). The
 * kernel answers BAD_REQUEST for any name it rejects; when a path is not valid
 * UTF-8, which native names must be, that is reported as EILSEQ instead of
 * EINVAL. This only classifies a failure and never changes what is accepted. */
int libc_path_errno(enum call_status status, const char *path, const char *other);

#endif
