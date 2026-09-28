#include "iobench.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned char fixture_byte(size_t offset)
{
  return (unsigned char)(offset ^ (offset >> 8) ^ (offset >> 16) ^ 0xa5);
}

void fill_fixture(unsigned char *bytes, size_t size, bool contrast)
{
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = fixture_byte(i) ^ (contrast ? 0xff : 0);
  }
}

bool measure_clock(handle_t clock, uint64_t *cost)
{
  uint64_t start, end;
  enum call_status status = clock_now(clock, &start);
  if (status == CALL_OK) {
    for (size_t i = 0; i < CLOCK_READS; ++i) {
      status = clock_now(clock, &end);
      if (status != CALL_OK) {
        break;
      }
    }
  }
  if (status != CALL_OK) {
    fprintf(stderr, "iobench: clock calibration failed (status %u)\n", status);
    return false;
  }
  if (end <= start) {
    fputs("iobench: clock calibration did not advance\n", stderr);
    return false;
  }
  *cost = (end - start) / CLOCK_READS;
  return true;
}

bool verify_fixture(const unsigned char *bytes, size_t size)
{
  /* The build-time fixture generator uses the same offset-dependent pattern. */
  for (size_t i = 0; i < size; ++i) {
    unsigned char expected = fixture_byte(i);
    if (bytes[i] != expected) {
      fprintf(stderr, "iobench: content mismatch at byte %zu (got %u, expected %u)\n",
          i, (unsigned)bytes[i], (unsigned)expected);
      return false;
    }
  }
  return true;
}

static int compare_time(const void *left, const void *right)
{
  uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

void print_summary(const char *name, uint64_t *times, size_t count, size_t bytes)
{
  qsort(times, count, sizeof(*times), compare_time);
  size_t middle = count / 2;
  double median = times[middle];
  if (!(count % 2)) {
    median = times[middle - 1] / 2.0 + times[middle] / 2.0;
  }
  fprintf(stderr, "%s summary: %zu verified samples; elapsed median=%.3f ms "
      "range=%.3f..%.3f ms", name, count, median / 1000000.0,
      times[0] / 1000000.0, times[count - 1] / 1000000.0);
  if (bytes) {
    fprintf(stderr, "; throughput at median elapsed=%.3f MiB/s", bytes * 1000000000.0 / (1024.0 * 1024.0) / median);
  }
  fputc('\n', stderr);
}
