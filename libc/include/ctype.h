#ifndef LIBC_CTYPE_H
#define LIBC_CTYPE_H

/* ASCII classification only, with no locale state. Arguments must be EOF or
 * representable as unsigned char; cast plain char before passing byte data.
 * EOF and bytes outside ASCII return zero. True results are nonzero. */
int isalpha(int character);
int isalnum(int character);
int iscntrl(int character);
int isgraph(int character);
int islower(int character);
int isupper(int character);
int ispunct(int character);
int isxdigit(int character);
int isspace(int character);
int isdigit(int character);
int isprint(int character);

/* ASCII case conversion; EOF and non-ASCII bytes are unchanged. */
int toupper(int character);
int tolower(int character);

#endif
