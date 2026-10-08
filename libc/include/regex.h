#ifndef LIBC_REGEX_H
#define LIBC_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long regoff_t;

typedef struct {
  size_t re_nsub;
  void *__opaque;
} regex_t;

typedef struct {
  regoff_t rm_so;
  regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8

#define REG_NOTBOL 1
#define REG_NOTEOL 2

#define REG_OK 0
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

#define REG_ENOSYS (-1)

/* A successful compilation owns heap storage until regfree; do not copy it.
 * Strings are UTF-8, match offsets count bytes, classes/folding are ASCII only.
 * The pinned TRE engine retains its UTF-8 and case-folding backreference limits. */
int regcomp(regex_t *__restrict pattern, const char *__restrict text, int flags);
int regexec(const regex_t *__restrict pattern, const char *__restrict text,
  size_t count, regmatch_t *__restrict matches, int flags);
void regfree(regex_t *pattern);

/* Returns required bytes including NUL; a nonzero buffer size always terminates.
 * A zero size permits a NULL buffer. The pattern may be NULL. */
size_t regerror(int error, const regex_t *__restrict pattern,
  char *__restrict buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
