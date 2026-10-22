#ifndef LIBC_LANGINFO_H
#define LIBC_LANGINFO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;

#define CODESET 1

/* Only CODESET is implemented. The native multibyte encoding is always UTF-8;
 * other items return an empty string. Returned storage must not be modified. */
char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif

#endif
