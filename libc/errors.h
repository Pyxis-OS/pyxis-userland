#ifndef LIBC_ERRORS_H
#define LIBC_ERRORS_H

#include <abi/syscall.h>

int libc_call_errno(enum call_status status);

#endif
