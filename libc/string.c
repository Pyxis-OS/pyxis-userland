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

int strcmp(const char *left, const char *right)
{
  while (*left && *left == *right) {
    ++left;
    ++right;
  }
  return (unsigned char)*left - (unsigned char)*right;
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
