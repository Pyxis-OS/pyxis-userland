#include <stdlib.h>

void *bsearch(const void *key, const void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *))
{
  const unsigned char *elements = base;

  while (count > 0) {
    size_t middle = count / 2;
    const unsigned char *element = elements + middle * size;
    int order = compare(key, element);

    if (order == 0) {
      return (void *)element;
    }

    if (order < 0) {
      count = middle;
    } else {
      elements = element + size;
      count -= middle + 1;
    }
  }

  return NULL;
}
