#ifndef LIBC_ICONV_H
#define LIBC_ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *iconv_t;

/* Conversion between UTF-8, ASCII (also US-ASCII, ISO646-US), ISO-8859-1
 * (also LATIN1) and UTF-16LE/UTF-16BE without a byte order mark. Names match
 * ignoring case and punctuation, so "UTF-8" and "utf8" are the same. Any other
 * name, including the empty locale name and //TRANSLIT or //IGNORE suffixes,
 * fails with EINVAL. No conversion allocates or keeps state, so iconv_close
 * always succeeds.
 *
 * iconv converts whole characters, advancing *input and *output and reducing
 * the counts as it goes, and returns zero. It stops with -1 and errno:
 * E2BIG when the next character does not fit in the output, EINVAL when the
 * input ends inside a character, and EILSEQ at an invalid input sequence or at
 * a valid character the destination cannot represent; *input then points at
 * that character. A null input or zero input count does nothing and returns
 * zero, which is the reset for these stateless conversions. */
iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t descriptor, char **__restrict input, size_t *__restrict input_left,
    char **__restrict output, size_t *__restrict output_left);
int iconv_close(iconv_t descriptor);

#ifdef __cplusplus
}
#endif

#endif
