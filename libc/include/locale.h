#ifndef LIBC_LOCALE_H
#define LIBC_LOCALE_H

#ifdef __cplusplus
extern "C" {
#endif

#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5

/* Only the "C" locale exists. A NULL locale queries and returns "C"; "" (the
 * native environment) and "C" select it and return "C". Any other name, or an
 * unknown category, returns NULL and leaves the locale unchanged. The returned
 * string must not be modified. There is no localeconv or lconv. */
char *setlocale(int category, const char *locale);

#ifdef __cplusplus
}
#endif

#endif
