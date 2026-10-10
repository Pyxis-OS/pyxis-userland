#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

size_t strlen(const char *text)
{
  size_t length = 0;
  while (text[length]) {
    ++length;
  }
  return length;
}

size_t strnlen(const char *text, size_t limit)
{
  size_t length = 0;
  while (length < limit && text[length]) {
    ++length;
  }
  return length;
}

char *strcpy(char *restrict dest, const char *restrict src)
{
  char *start = dest;
  while (*src) {
    *dest++ = *src++;
  }
  *dest = '\0';
  return start;
}

char *stpcpy(char *restrict dest, const char *restrict src)
{
  while (*src) {
    *dest++ = *src++;
  }
  *dest = '\0';
  return dest;
}

char *strcat(char *restrict dest, const char *restrict src)
{
  strcpy(dest + strlen(dest), src);
  return dest;
}

char *strncat(char *restrict dest, const char *restrict src, size_t count)
{
  char *end = dest + strlen(dest);
  size_t length = strnlen(src, count);
  memcpy(end, src, length);
  end[length] = '\0';
  return dest;
}

int strcmp(const char *left, const char *right)
{
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return (unsigned char)*left - (unsigned char)*right;
}

int strcoll(const char *left, const char *right)
{
  return strcmp(left, right);
}

int strncmp(const char *left, const char *right, size_t limit)
{
  for (size_t i = 0; i < limit; ++i) {
    if (left[i] != right[i] || !left[i]) {
      return (unsigned char)left[i] - (unsigned char)right[i];
    }
  }
  return 0;
}

char *strchr(const char *text, int character)
{
  do {
    if (*text == (char)character) {
      return (char *)text;
    }
  } while (*text++);
  return NULL;
}

char *strchrnul(const char *text, int character)
{
  while (*text && *text != (char)character) {
    ++text;
  }
  return (char *)text;
}

char *strrchr(const char *text, int character)
{
  const char *last = NULL;
  do {
    if (*text == (char)character) {
      last = text;
    }
  } while (*text++);
  return (char *)last;
}

size_t strspn(const char *text, const char *accept)
{
  size_t length = 0;
  while (text[length] && strchr(accept, text[length])) {
    ++length;
  }
  return length;
}

size_t strcspn(const char *text, const char *reject)
{
  size_t length = 0;
  while (text[length] && !strchr(reject, text[length])) {
    ++length;
  }
  return length;
}

char *strpbrk(const char *text, const char *accept)
{
  for (; *text; ++text) {
    if (strchr(accept, *text)) {
      return (char *)text;
    }
  }
  return NULL;
}

char *strtok_r(char *restrict text, const char *restrict separators,
    char **restrict state)
{
  if (!text) {
    text = *state;
    if (!text) {
      return NULL;
    }
  }
  text += strspn(text, separators);
  if (!*text) {
    *state = NULL;
    return NULL;
  }
  char *end = text + strcspn(text, separators);
  *state = *end ? end + 1 : NULL;
  *end = '\0';
  return text;
}

char *strstr(const char *text, const char *needle)
{
  if (!*needle) {
    return (char *)text;
  }

  for (; *text; ++text) {
    const char *candidate = text;
    const char *match = needle;
    while (*match && *candidate && *candidate == *match) {
      ++candidate;
      ++match;
    }
    if (!*match) {
      return (char *)text;
    }
  }
  return NULL;
}

char *strcasestr(const char *text, const char *needle)
{
  if (!*needle) {
    return (char *)text;
  }

  for (; *text; ++text) {
    const char *candidate = text;
    const char *match = needle;
    while (*match && *candidate &&
           tolower((unsigned char)*candidate) == tolower((unsigned char)*match)) {
      ++candidate;
      ++match;
    }
    if (!*match) {
      return (char *)text;
    }
  }
  return NULL;
}

char *strndup(const char *text, size_t limit)
{
  size_t length = strnlen(text, limit);
  if (length == SIZE_MAX) {
    errno = ENOMEM;
    return NULL;
  }
  char *copy = malloc(length + 1);
  if (copy) {
    memcpy(copy, text, length);
    copy[length] = '\0';
  }
  return copy;
}

char *strdup(const char *text)
{
  return strndup(text, SIZE_MAX);
}

char *strncpy(char *restrict dest, const char *restrict src, size_t count)
{
  size_t copied = 0;
  while (copied < count && src[copied]) {
    dest[copied] = src[copied];
    ++copied;
  }
  while (copied < count) {
    dest[copied++] = '\0';
  }
  return dest;
}
