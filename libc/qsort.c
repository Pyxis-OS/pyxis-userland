#include <stdlib.h>

static void swap_elements(unsigned char *left, unsigned char *right, size_t size)
{
  for (size_t byte = 0; byte < size; ++byte) {
    unsigned char temporary = left[byte];
    left[byte] = right[byte];
    right[byte] = temporary;
  }
}

static void sift_down(unsigned char *base, size_t root, size_t count,
  size_t size, int (*compare)(const void *, const void *))
{
  /* Only internal nodes have children; this also keeps 2 * root + 1 in range. */
  while (root < count / 2) {
    size_t child = 2 * root + 1;

    if (child + 1 < count &&
        compare(base + child * size, base + (child + 1) * size) < 0) {
      ++child;
    }

    if (compare(base + root * size, base + child * size) >= 0) {
      return;
    }

    swap_elements(base + root * size, base + child * size, size);
    root = child;
  }
}

void qsort(void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *))
{
  if (count < 2 || size == 0) {
    return;
  }

  unsigned char *elements = base;

  /* Build a max heap from the bottom up, then move each maximum to the end. */
  for (size_t parent = count / 2; parent > 0; --parent) {
    sift_down(elements, parent - 1, count, size, compare);
  }

  for (size_t remaining = count - 1; remaining > 0; --remaining) {
    swap_elements(elements, elements + remaining * size, size);
    sift_down(elements, 0, remaining, size, compare);
  }
}
