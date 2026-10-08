#ifndef LIBC_STRINGS_H
#define LIBC_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ASCII case folding, comparing bytes as unsigned char; no locale state. */
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t count);

#ifdef __cplusplus
}
#endif

#endif
