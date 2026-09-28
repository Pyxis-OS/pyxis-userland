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

static void report_host_durations(const char *first_name,
    const struct profile_duration *first, const char *second_name,
    const struct profile_duration *second)
{
  fprintf(stderr, "    %s=%llu/%llu %s=%llu/%llu ns (sum/max)\n",
      first_name, (unsigned long long)first->total_ns,
      (unsigned long long)first->maximum_ns, second_name,
      (unsigned long long)second->total_ns, (unsigned long long)second->maximum_ns);
}

static void report_host_operation(const char *name, const struct profile_host_operation *operation)
{
  if (!operation->requests) {
    fprintf(stderr, "  HOST %s: no requests\n", name);
    return;
  }
  fprintf(stderr, "  HOST %s: requests=%llu failed=%llu short=%llu eof=%llu\n", name,
      (unsigned long long)operation->requests, (unsigned long long)operation->failures,
      (unsigned long long)operation->short_transfers, (unsigned long long)operation->eof);
  fprintf(stderr, "    requested=%llu completed=%llu bytes\n",
      (unsigned long long)operation->requested_bytes,
      (unsigned long long)operation->completed_bytes);
  fprintf(stderr, "    transport: submitted=%llu completed=%llu failed=%llu\n",
      (unsigned long long)operation->submissions, (unsigned long long)operation->completions,
      (unsigned long long)operation->transport_failures);
  report_host_durations("publication", &operation->publication, "bsp_queue", &operation->bsp_queue);
  report_host_durations("worker_queue", &operation->worker_queue, "service", &operation->service);
  report_host_durations("resume", &operation->resume, "total", &operation->total);
  report_host_durations("transport", &operation->transport, "transport_failed", &operation->transport_failed);
}

void report_host_profile(const struct profile_host_snapshot *profile)
{
  report_host_operation("read", &profile->read);
  report_host_operation("write", &profile->write);
  if (profile->flags & PROFILE_SATURATED) {
    fputs("  HOST profile counters saturated\n", stderr);
  }
}
