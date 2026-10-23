#ifndef LIBC_ALLOCA_H
#define LIBC_ALLOCA_H

/* Compiler-managed stack storage, released when the calling function returns. */
#define alloca(size) __builtin_alloca(size)

#endif
