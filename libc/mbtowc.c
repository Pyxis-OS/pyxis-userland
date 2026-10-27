#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

int mbtowc(wchar_t *restrict output, const char *restrict text, size_t size)
{
  if (!text) {
    return 0;
  }

  mbstate_t state = {0};
  size_t count = mbrtowc(output, text, size, &state);
  if (count == (size_t)-2) {
    errno = EILSEQ;
    return -1;
  }
  return count == (size_t)-1 ? -1 : (int)count;
}

int mblen(const char *text, size_t size)
{
  return mbtowc(NULL, text, size);
}
