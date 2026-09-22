#include <string.h>
#include <stdint.h>

void *memset(void *dest, int value, size_t count)
{
  unsigned char *dest_bytes = dest;
  for (size_t i = 0; i < count; ++i) {
    dest_bytes[i] = (unsigned char)value;
  }
  return dest;
}

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
  unsigned char *dest_bytes = dest;
  const unsigned char *src_bytes = src;
  for (size_t i = 0; i < count; ++i) {
    dest_bytes[i] = src_bytes[i];
  }
  return dest;
}

void *memmove(void *dest, const void *src, size_t count)
{
  unsigned char *dest_bytes = dest;
  const unsigned char *src_bytes = src;
  if ((uintptr_t)dest_bytes < (uintptr_t)src_bytes) {
    for (size_t i = 0; i < count; ++i) {
      dest_bytes[i] = src_bytes[i];
    }
  } else {
    /* Copy backward so an overlapping destination cannot overwrite unread data. */
    while (count) {
      --count;
      dest_bytes[count] = src_bytes[count];
    }
  }
  return dest;
}

int memcmp(const void *a, const void *b, size_t count)
{
  const unsigned char *left_bytes = a;
  const unsigned char *right_bytes = b;
  for (size_t i = 0; i < count; ++i) {
    if (left_bytes[i] != right_bytes[i]) {
      return (int)left_bytes[i] - (int)right_bytes[i];
    }
  }
  return 0;
}
