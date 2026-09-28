#include <clock.h>
#include <errno.h>
#include <fcntl.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIXTURE_BYTES (1024 * 1024)
#define MAX_BUFFER_BYTES 65536
#define MAX_ROUNDS 100
#define CLOCK_READS 1000

struct options {
  const char *uri;
  size_t buffer, rounds;
};

struct result {
  size_t bytes, calls, short_reads;
  uint64_t elapsed;
  bool timed;
};

static bool number(const char *text, size_t maximum, size_t *result)
{
  size_t value = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    size_t digit = *text - '0';
    if (value > (maximum - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *result = value;
  return value != 0;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
  if (argc < 3 || strcmp(argv[1], "read") || !*argv[2]) {
    return false;
  }
  *options = (struct options){argv[2], 4088, 5};
  bool have_buffer = false, have_rounds = false;
  for (int i = 3; i < argc; i += 2) {
    if (i + 1 == argc) {
      return false;
    }
    if (!strcmp(argv[i], "--buffer") && !have_buffer) {
      have_buffer = true;
      if (!number(argv[i + 1], MAX_BUFFER_BYTES, &options->buffer)) {
        return false;
      }
    } else if (!strcmp(argv[i], "--rounds") && !have_rounds) {
      have_rounds = true;
      if (!number(argv[i + 1], MAX_ROUNDS, &options->rounds)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

static bool measure_clock(handle_t clock, uint64_t *cost)
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

static bool verify(const unsigned char *bytes)
{
  /* The build-time fixture generator uses the same offset-dependent pattern. */
  for (size_t i = 0; i < FIXTURE_BYTES; ++i) {
    unsigned char expected = (unsigned char)(i ^ (i >> 8) ^ (i >> 16) ^ 0xa5);
    if (bytes[i] != expected) {
      fprintf(stderr, "iobench: content mismatch at byte %zu (got %u, expected %u)\n",
          i, (unsigned)bytes[i], (unsigned)expected);
      return false;
    }
  }
  return true;
}

static bool read_pass(const struct options *options, unsigned char *bytes,
    handle_t clock, bool timed, struct result *result)
{
  *result = (struct result){0};
  memset(bytes, 0, FIXTURE_BYTES);
  int descriptor = open(options->uri, O_RDONLY);
  if (descriptor < 0) {
    fprintf(stderr, "iobench: open failed (errno %d)\n", errno);
    return false;
  }

  uint64_t start = 0, end = 0;
  enum call_status clock_status = CALL_OK;
  if (timed) {
    clock_status = clock_now(clock, &start);
  }
  int read_error = 0;
  bool early_eof = false;
  if (clock_status == CALL_OK) {
    while (result->bytes < FIXTURE_BYTES) {
      size_t request = FIXTURE_BYTES - result->bytes;
      if (request > options->buffer) {
        request = options->buffer;
      }
      ++result->calls;
      ssize_t count = read(descriptor, bytes + result->bytes, request);
      if (count < 0) {
        read_error = errno;
        break;
      }
      if (!count) {
        early_eof = true;
        break;
      }
      if ((size_t)count < request) {
        ++result->short_reads;
      }
      result->bytes += (size_t)count;
    }
    if (timed) {
      clock_status = clock_now(clock, &end);
      if (clock_status == CALL_OK && end > start) {
        result->elapsed = end - start;
        result->timed = true;
      }
    }
  }

  bool success = true;
  if (clock_status != CALL_OK) {
    fprintf(stderr, "iobench: clock failed (status %u)\n", clock_status);
    success = false;
  } else if (timed && !result->timed) {
    fputs("iobench: measured clock did not advance\n", stderr);
    success = false;
  }
  if (read_error) {
    fprintf(stderr, "iobench: read failed (errno %d)\n", read_error);
    success = false;
  }
  if (early_eof) {
    fputs("iobench: fixture ended before 1048576 bytes\n", stderr);
    success = false;
  }
  if (success) {
    /* Size/content checks and close are outside the timed payload loop. */
    unsigned char extra;
    ssize_t count = read(descriptor, &extra, 1);
    if (count < 0) {
      fprintf(stderr, "iobench: EOF check failed (errno %d)\n", errno);
      success = false;
    } else if (count) {
      fputs("iobench: fixture exceeds 1048576 bytes\n", stderr);
      success = false;
    }
    if (success) {
      success = verify(bytes);
    }
  }
  if (close(descriptor) < 0) {
    fprintf(stderr, "iobench: close failed (errno %d); not retried\n", errno);
    success = false;
  }
  return success;
}

static void report(const char *phase, size_t pass, const struct result *result,
    bool success)
{
  fprintf(stderr, "%s %zu: %s; requested=%u completed=%zu bytes; "
      "read_calls=%zu short_reads=%zu failed_pass=%u\n", phase, pass,
      success ? "OK" : "FAILED", FIXTURE_BYTES, result->bytes,
      result->calls, result->short_reads, success ? 0U : 1U);
  if (result->timed) {
    fprintf(stderr, "  elapsed=%llu ns (%.3f ms)",
        (unsigned long long)result->elapsed, result->elapsed / 1000000.0);
    if (success) {
      fprintf(stderr, "; %.3f MiB/s", result->bytes * 1000000000.0 /
          result->elapsed / (1024.0 * 1024.0));
    }
    fputc('\n', stderr);
  }
}

static int compare_time(const void *left, const void *right)
{
  uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
  return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: iobench read URI [--buffer bytes] [--rounds count]\n"
          "buffer: 1..65536 (default 4088); rounds: 1..100 (default 5)\n"
          "URI must contain the exact 1 MiB share/iobench.bin fixture\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t clock = startup_resource("clock");
  if (clock == HANDLE_INVALID) {
    fputs("iobench: missing clock grant\n", stderr);
    return EXIT_FAILURE;
  }
  unsigned char *bytes = malloc(FIXTURE_BYTES);
  if (!bytes) {
    fputs("iobench: cannot allocate 1 MiB result buffer\n", stderr);
    return EXIT_FAILURE;
  }
  uint64_t times[MAX_ROUNDS], clock_cost;
  bool success = measure_clock(clock, &clock_cost);
  if (success) {
    fprintf(stderr, "iobench read: uri=%s buffer=%zu rounds=%zu warmup=1 "
        "fixture=%u bytes\n", options.uri, options.buffer, options.rounds, FIXTURE_BYTES);
    fprintf(stderr, "Clock-call loop: %llu ns/read (%u reads); not subtracted\n",
        (unsigned long long)clock_cost, CLOCK_READS);
    struct result result;
    success = read_pass(&options, bytes, clock, false, &result);
    report("warmup", 1, &result, success);
    for (size_t i = 0; success && i < options.rounds; ++i) {
      success = read_pass(&options, bytes, clock, true, &result);
      report("sample", i + 1, &result, success);
      times[i] = result.elapsed;
    }
  }
  if (success) {
    qsort(times, options.rounds, sizeof(*times), compare_time);
    size_t middle = options.rounds / 2;
    double median = times[middle];
    if (!(options.rounds % 2)) {
      median = times[middle - 1] / 2.0 + times[middle] / 2.0;
    }
    fprintf(stderr, "Summary: %zu verified samples; elapsed median=%.3f ms "
        "range=%.3f..%.3f ms; throughput at median elapsed=%.3f MiB/s\n", options.rounds,
        median / 1000000.0, times[0] / 1000000.0,
        times[options.rounds - 1] / 1000000.0, 1000000000.0 / median);
  } else {
    fputs("iobench: FAILED; no successful-run summary\n", stderr);
  }
  free(bytes);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
