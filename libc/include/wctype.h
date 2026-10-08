#ifndef LIBC_WCTYPE_H
#define LIBC_WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long wctype_t;

/* Every non-ASCII value, including WEOF, is outside all classes. */
int iswalnum(wint_t character);
int iswalpha(wint_t character);
int iswblank(wint_t character);
int iswcntrl(wint_t character);
int iswdigit(wint_t character);
int iswgraph(wint_t character);
int iswlower(wint_t character);
int iswprint(wint_t character);
int iswpunct(wint_t character);
int iswspace(wint_t character);
int iswupper(wint_t character);
int iswxdigit(wint_t character);

/* ASCII case conversion; every other value is unchanged. */
wint_t towlower(wint_t character);
wint_t towupper(wint_t character);

/* The twelve standard ASCII class names yield immutable descriptors.
 * Unknown names return zero. iswctype also returns zero for descriptor zero. */
wctype_t wctype(const char *name);
int iswctype(wint_t character, wctype_t property);

#ifdef __cplusplus
}
#endif

#endif
