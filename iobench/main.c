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

#include "iobench.h"

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
  if (argc < 2) {
    return false;
  }
  *options = (struct options){.buffer = 4080, .rounds = 5};
  int first_option = 3;
  if (!strcmp(argv[1], "pipe")) {
    options->mode = IO_PIPE;
    options->buffer = 4096;
    first_option = 2;
  } else if (argc < 3 || !*argv[2]) {
    return false;
  } else if (!strcmp(argv[1], "read")) {
    options->mode = IO_READ;
    options->source = argv[2];
    options->buffer = 4088;
  } else if (!strcmp(argv[1], "write")) {
    options->mode = IO_WRITE;
    options->output = argv[2];
  } else if (!strcmp(argv[1], "copy") && argc >= 4 && *argv[3]) {
    options->mode = IO_COPY;
    options->source = argv[2];
    options->output = argv[3];
    first_option = 4;
  } else {
    return false;
  }

  bool have_buffer = false, have_rounds = false;
  for (int i = first_option; i < argc; ++i) {
    const char *option = argv[i];
    bool output = options->mode == IO_WRITE || options->mode == IO_COPY;
    if (!strcmp(option, "--prepared") && output && !options->prepared) {
      options->prepared = true;
    } else if (!strcmp(option, "--sync") && output && !options->sync) {
      options->sync = true;
    } else {
      if (++i == argc) {
        return false;
      }
      if (!strcmp(option, "--buffer") && !have_buffer) {
        have_buffer = true;
        if (!number(argv[i], MAX_BUFFER_BYTES, &options->buffer)) {
          return false;
        }
      } else if (!strcmp(option, "--rounds") && !have_rounds) {
        have_rounds = true;
        if (!number(argv[i], MAX_ROUNDS, &options->rounds)) {
          return false;
        }
      } else {
        return false;
      }
    }
  }
  return true;
}

static bool read_pass(const struct options *options, unsigned char *bytes,
    handle_t clock, bool timed, struct result *result)
{
  *result = (struct result){0};
  memset(bytes, 0, FIXTURE_BYTES);
  int descriptor = open(options->source, O_RDONLY);
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
      success = verify_fixture(bytes);
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

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "--pipe-worker")) {
    return pipe_worker(argc, argv);
  }
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: iobench read SOURCE [--buffer bytes] [--rounds count]\n"
          "       iobench write OUTPUT [--prepared] [--sync] [--buffer bytes] [--rounds count]\n"
          "       iobench copy SOURCE OUTPUT [--prepared] [--sync] [--buffer bytes] [--rounds count]\n"
          "       session app://iobench.pxe pipe [--buffer bytes] [--rounds count]\n"
          "buffer: 1..65536 (default read=4088, write/copy=4080, pipe=4096); rounds: 1..100 (default 5)\n"
          "SOURCE must contain the exact 1 MiB share/iobench.bin fixture\n"
          "OUTPUT must be a new file; it is retained even on failure\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t clock = startup_resource("clock");
  if (clock == HANDLE_INVALID) {
    fputs("iobench: missing clock grant\n", stderr);
    return EXIT_FAILURE;
  }
  unsigned char *bytes = options.mode == IO_PIPE ? NULL : malloc(FIXTURE_BYTES);
  if (options.mode != IO_PIPE && !bytes) {
    fputs("iobench: cannot allocate 1 MiB result buffer\n", stderr);
    return EXIT_FAILURE;
  }
  uint64_t times[MAX_ROUNDS], clock_cost;
  bool success = measure_clock(clock, &clock_cost);
  if (success) {
    fprintf(stderr, "iobench %s: source=%s output=%s buffer=%zu rounds=%zu warmup=1 "
        "fixture=%u bytes\n", argv[1], options.source ? options.source : "generated",
        options.output ? options.output : "none", options.buffer, options.rounds, FIXTURE_BYTES);
    fprintf(stderr, "Clock-call loop: %llu ns/read (%u reads); not subtracted\n",
        (unsigned long long)clock_cost, CLOCK_READS);
    if (options.mode == IO_PIPE) {
      success = run_pipe(&options, clock);
    } else if (options.mode != IO_READ) {
      success = run_output(&options, bytes, clock);
    } else {
      struct result result;
      success = read_pass(&options, bytes, clock, false, &result);
      report("warmup", 1, &result, success);
      for (size_t i = 0; success && i < options.rounds; ++i) {
        success = read_pass(&options, bytes, clock, true, &result);
        report("sample", i + 1, &result, success);
        times[i] = result.elapsed;
      }
      if (success) {
        print_summary("Read", times, options.rounds, true);
      }
    }
  }
  if (!success) {
    fputs("iobench: FAILED; no successful-run summary\n", stderr);
  }
  free(bytes);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
