#include <errno.h>
#include <stdlib.h>

int mbtowc(wchar_t *restrict output, const char *restrict text, size_t size)
{
  if (!text) {
    return 0;
  }
  if (!size) {
    errno = EILSEQ;
    return -1;
  }

  const unsigned char *bytes = (const unsigned char *)text;
  unsigned value = bytes[0];
  unsigned minimum = 0;
  unsigned count = 1;
  if (value >= 0xc2 && value <= 0xdf) {
    value &= 0x1f;
    minimum = 0x80;
    count = 2;
  } else if (value >= 0xe0 && value <= 0xef) {
    value &= 0x0f;
    minimum = 0x800;
    count = 3;
  } else if (value >= 0xf0 && value <= 0xf4) {
    value &= 0x07;
    minimum = 0x10000;
    count = 4;
  } else if (value >= 0x80) {
    errno = EILSEQ;
    return -1;
  }

  if (size < count) {
    errno = EILSEQ;
    return -1;
  }
  for (unsigned i = 1; i < count; ++i) {
    if (bytes[i] < 0x80 || bytes[i] > 0xbf) {
      errno = EILSEQ;
      return -1;
    }
    value = (value << 6) | (bytes[i] & 0x3f);
  }
  if (value < minimum || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    errno = EILSEQ;
    return -1;
  }
  if (output) {
    *output = (wchar_t)value;
  }
  return value ? (int)count : 0;
}
