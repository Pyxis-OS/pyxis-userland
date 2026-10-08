#ifndef LIBC_WCHAR_H
#define LIBC_WCHAR_H

#include <stddef.h>

typedef unsigned int wint_t;
#define WEOF ((wint_t)-1)

/* Conversion state for restartable multibyte functions. libc's UTF-8
 * conversion is stateless and no function uses this type yet. */
typedef struct {
  unsigned int reserved;
} mbstate_t;

#endif
