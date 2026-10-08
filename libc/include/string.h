#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *__restrict dest, const void *__restrict src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *memory, int value, size_t count);
/* Last occurrence within count bytes; NULL when absent. */
void *memrchr(const void *memory, int value, size_t count);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
/* Copy through NUL; dest has enough space and does not overlap src. */
char *strcpy(char *__restrict dest, const char *__restrict src);
/* As strcpy, but returns the address of the copied NUL in dest. */
char *stpcpy(char *__restrict dest, const char *__restrict src);
/* Copies exactly count bytes, padding with NUL after the source ends.
 * Does not terminate when the source is count bytes or longer. No overlap. */
char *strncpy(char *__restrict dest, const char *__restrict src, size_t count);
char *strcat(char *__restrict dest, const char *__restrict src);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t limit);
char *strchr(const char *text, int character);
/* As strchr, but returns the terminating NUL instead of NULL when absent. */
char *strchrnul(const char *text, int character);
char *strrchr(const char *text, int character);
size_t strspn(const char *text, const char *accept);
size_t strcspn(const char *text, const char *reject);
char *strpbrk(const char *text, const char *accept);
/* First occurrence; an empty needle returns text. NULL when not found. */
char *strstr(const char *text, const char *needle);
/* Same search with ASCII case folding; bytes outside ASCII are unchanged. */
char *strcasestr(const char *text, const char *needle);
char *strdup(const char *text);
char *strndup(const char *text, size_t limit);

/* Static message storage; callers must not modify it. */
char *strerror(int error);

#ifdef __cplusplus
}
#endif

#endif
