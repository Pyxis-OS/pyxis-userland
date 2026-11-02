#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

static void heap_sort(unsigned char *elements, size_t count, size_t size,
  int (*compare)(const void *, const void *))
{
  /* Build a max heap from the bottom up, then move each maximum to the end. */
  for (size_t parent = count / 2; parent > 0; --parent) {
    sift_down(elements, parent - 1, count, size, compare);
  }

  for (size_t remaining = count - 1; remaining > 0; --remaining) {
    swap_elements(elements, elements + remaining * size, size);
    sift_down(elements, 0, remaining, size, compare);
  }
}

/* One bottom-up pass: merge adjacent runs of WIDTH from the array into the
 * buffer. Equal elements keep their order because the left run wins ties. */
static void merge_pass(const unsigned char *elements, unsigned char *buffer, size_t count,
  size_t size, size_t width, int (*compare)(const void *, const void *))
{
  for (size_t left = 0; left < count; left += 2 * width) {
    size_t middle = count - left > width ? left + width : count;
    size_t right = count - middle > width ? middle + width : count;
    size_t i = left, j = middle, out = left;
    while (i < middle && j < right) {
      if (compare(elements + j * size, elements + i * size) < 0) {
        memcpy(buffer + out++ * size, elements + j++ * size, size);
      } else {
        memcpy(buffer + out++ * size, elements + i++ * size, size);
      }
    }
    memcpy(buffer + out * size, elements + i * size, (middle - i) * size);
    out += middle - i;
    memcpy(buffer + out * size, elements + j * size, (right - j) * size);
  }
}

void qsort(void *base, size_t count, size_t size,
  int (*compare)(const void *, const void *))
{
  if (count < 2 || size == 0) {
    return;
  }

  unsigned char *elements = base;
  /* Stable merge sort. Each pass is copied back before the next, so the
   * comparison only ever sees elements of the array, as ISO C requires.
   * Without memory for the buffer the in-place heapsort runs, unstably. */
  unsigned char *buffer = count <= SIZE_MAX / size ? malloc(count * size) : NULL;
  if (!buffer) {
    heap_sort(elements, count, size, compare);
    return;
  }
  for (size_t width = 1; width < count; width *= 2) {
    merge_pass(elements, buffer, count, size, width, compare);
    memcpy(elements, buffer, count * size);
    if (width > count / 2) {
      break;
    }
  }
  free(buffer);
}
