#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *memory, int value, size_t count);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
/* Copy through NUL; dest has enough space and does not overlap src. */
char *strcpy(char *restrict dest, const char *restrict src);
/* Copies exactly count bytes, padding with NUL after the source ends.
 * Does not terminate when the source is count bytes or longer. No overlap. */
char *strncpy(char *restrict dest, const char *restrict src, size_t count);
char *strcat(char *restrict dest, const char *restrict src);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t limit);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
size_t strspn(const char *text, const char *accept);
char *strpbrk(const char *text, const char *accept);
/* First occurrence; an empty needle returns text. NULL when not found. */
char *strstr(const char *text, const char *needle);
/* Same search with ASCII case folding; bytes outside ASCII are unchanged. */
char *strcasestr(const char *text, const char *needle);
char *strdup(const char *text);
char *strndup(const char *text, size_t limit);

/* Static message storage; callers must not modify it. */
char *strerror(int error);

#endif
