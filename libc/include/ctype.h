#ifndef LIBC_CTYPE_H
#define LIBC_CTYPE_H

/* ASCII classification only, with no locale state. Arguments must be EOF or
 * representable as unsigned char; cast plain char before passing byte data.
 * EOF and bytes outside ASCII return zero. True results are nonzero. */
int isspace(int character);
int isdigit(int character);
int isprint(int character);

#endif
