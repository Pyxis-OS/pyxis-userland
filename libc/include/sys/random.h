#ifndef LIBC_SYS_RANDOM_H
#define LIBC_SYS_RANDOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow startup random/clock authority; no fallback or partial success.
 * Up to 256 bytes, with a checked absolute deadline five seconds ahead.
 * Oversize is EIO, unavailable authority fails, and every failure preserves
 * the output. Zero length still validates authority and deadline. */
int getentropy(void *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
