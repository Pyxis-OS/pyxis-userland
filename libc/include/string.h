#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t limit);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
/* First occurrence; an empty needle returns text. NULL when not found. */
char *strstr(const char *text, const char *needle);
char *strdup(const char *text);
char *strndup(const char *text, size_t limit);

/* Static message storage; callers must not modify it. */
char *strerror(int error);

#endif
