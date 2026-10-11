#include <string.h>

size_t strxfrm(char *restrict output, const char *restrict text, size_t capacity)
{
  size_t length = strlen(text);
  size_t copied = length < capacity ? length + 1 : capacity;
  if (copied) {
    memcpy(output, text, copied);
  }
  return length;
}
