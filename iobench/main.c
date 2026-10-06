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
  uint64_t open_elapsed, elapsed, complete_elapsed;
  bool open_timed, timed, complete_timed, eof_called, host_profiled;
  struct profile_host_snapshot host_profile;
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
  *options = (struct options){.buffer = 4080, .rounds = 5, .bytes = FIXTURE_BYTES};
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

  bool have_buffer = false, have_rounds = false, have_bytes = false;
  for (int i = first_option; i < argc; ++i) {
    const char *option = argv[i];
    bool output = options->mode == IO_WRITE || options->mode == IO_COPY;
    if (!strcmp(option, "--prepared") && output && !options->prepared) {
      options->prepared = true;
    } else if (!strcmp(option, "--profile") && output && !options->profile) {
      options->profile = true;
    } else if (!strcmp(option, "--host-profile") && options->mode != IO_PIPE &&
        !options->host_profile) {
      options->host_profile = true;
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
      } else if (!strcmp(option, "--bytes") && options->mode == IO_READ && !have_bytes) {
        have_bytes = true;
        if (!number(argv[i], FIXTURE_BYTES, &options->bytes)) {
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
    handle_t clock, handle_t profile, bool timed, struct result *result)
{
  *result = (struct result){0};
  fill_fixture(bytes, options->bytes, true);
  bool host_profiled = timed && options->host_profile;
  enum call_status profile_end_status = CALL_OK;
  if (host_profiled) {
    enum call_status status = profile_host_begin(profile);
    if (status != CALL_OK) {
      fprintf(stderr, "iobench: host profile begin failed (status %u)\n", status);
      return false;
    }
  }
  uint64_t start = 0, opened = 0, read_end = 0, complete = 0;
  enum call_status clock_status = timed ? clock_now(clock, &start) : CALL_OK;
  if (clock_status != CALL_OK) {
    if (host_profiled) {
      profile_end_status = profile_host_end(profile, &result->host_profile);
      result->host_profiled = profile_end_status == CALL_OK;
      if (profile_end_status != CALL_OK) {
        fprintf(stderr, "iobench: host profile end failed (status %u)\n", profile_end_status);
      }
    }
    fprintf(stderr, "iobench: clock failed before OPEN (status %u)\n", clock_status);
    return false;
  }

  int descriptor = open(options->source, O_RDONLY);
  int open_error = descriptor < 0 ? errno : 0;
  if (timed) {
    clock_status = clock_now(clock, &opened);
    if (clock_status == CALL_OK && opened > start) {
      result->open_elapsed = opened - start;
      result->open_timed = true;
    }
  }

  int read_error = 0, eof_error = 0, close_error = 0;
  bool early_eof = false, excess = false;
  if (descriptor >= 0 && clock_status == CALL_OK) {
    while (result->bytes < options->bytes) {
      size_t request = options->bytes - result->bytes;
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
      clock_status = clock_now(clock, &read_end);
      if (clock_status == CALL_OK && read_end > opened) {
        result->elapsed = read_end - opened;
        result->timed = true;
      }
    }
  }
  if (host_profiled) {
    profile_end_status = profile_host_end(profile, &result->host_profile);
    result->host_profiled = profile_end_status == CALL_OK;
  }
  if (descriptor >= 0 && !read_error && !early_eof && clock_status == CALL_OK &&
      profile_end_status == CALL_OK) {
    unsigned char extra;
    result->eof_called = true;
    ssize_t count = read(descriptor, &extra, 1);
    if (count < 0) {
      eof_error = errno;
    } else {
      excess = count != 0;
    }
  }
  if (descriptor >= 0 && close(descriptor) < 0) {
    close_error = errno;
  }
  if (timed && clock_status == CALL_OK) {
    clock_status = clock_now(clock, &complete);
    if (clock_status == CALL_OK && complete > read_end && read_end > opened) {
      result->complete_elapsed = complete - start;
      result->complete_timed = true;
    }
  }

  /* Diagnostics and content verification follow the final timestamp. The
   * complete interval includes EOF and close, not deferred provider retirement. */
  bool success = true;
  if (profile_end_status != CALL_OK) {
    fprintf(stderr, "iobench: host profile end failed (status %u)\n", profile_end_status);
    success = false;
  }
  if (clock_status != CALL_OK) {
    fprintf(stderr, "iobench: clock failed (status %u)\n", clock_status);
    success = false;
  } else if (timed && descriptor >= 0 &&
      (!result->open_timed || !result->timed || !result->complete_timed)) {
    fputs("iobench: measured clock did not advance\n", stderr);
    success = false;
  }
  if (open_error) {
    fprintf(stderr, "iobench: open failed (errno %d)\n", open_error);
    success = false;
  }
  if (read_error) {
    fprintf(stderr, "iobench: read failed (errno %d)\n", read_error);
    success = false;
  }
  if (early_eof) {
    fprintf(stderr, "iobench: fixture ended before %zu bytes\n", options->bytes);
    success = false;
  }
  if (eof_error) {
    fprintf(stderr, "iobench: EOF check failed (errno %d)\n", eof_error);
    success = false;
  }
  if (excess) {
    fprintf(stderr, "iobench: fixture exceeds %zu bytes\n", options->bytes);
    success = false;
  }
  if (close_error) {
    fprintf(stderr, "iobench: close failed (errno %d); not retried\n", close_error);
    success = false;
  }
  return success && verify_fixture(bytes, options->bytes);
}

static void report(const char *phase, size_t pass, size_t requested,
    const struct result *result, bool success)
{
  fprintf(stderr, "%s %zu: %s; requested=%zu completed=%zu bytes; "
      "read_calls=%zu short_reads=%zu eof_calls=%u failed_pass=%u\n", phase, pass,
      success ? "OK" : "FAILED", requested, result->bytes,
      result->calls, result->short_reads, result->eof_called ? 1U : 0U, success ? 0U : 1U);
  if (result->open_timed) {
    fprintf(stderr, "  open=%llu ns\n", (unsigned long long)result->open_elapsed);
  }
  if (result->timed) {
    fprintf(stderr, "  payload=%llu ns (%.3f ms)",
        (unsigned long long)result->elapsed, result->elapsed / 1000000.0);
    if (success) {
      fprintf(stderr, "; %.3f MiB/s", result->bytes * 1000000000.0 /
          result->elapsed / (1024.0 * 1024.0));
    }
    fputc('\n', stderr);
  }
  if (result->complete_timed) {
    fprintf(stderr, "  complete=%llu ns (%.3f ms)",
        (unsigned long long)result->complete_elapsed, result->complete_elapsed / 1000000.0);
    if (success) {
      fprintf(stderr, "; %.3f MiB/s", result->bytes * 1000000000.0 /
          result->complete_elapsed / (1024.0 * 1024.0));
    }
    fputc('\n', stderr);
  }
  if (result->host_profiled) {
    report_host_profile(&result->host_profile);
  }
}

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "--pipe-worker")) {
    return pipe_worker(argc, argv);
  }
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: iobench read SOURCE [--host-profile] [--bytes count] [--buffer bytes] [--rounds count]\n"
          "       iobench write OUTPUT [--prepared] [--sync] [--profile] [--host-profile] [--buffer bytes] [--rounds count]\n"
          "       iobench copy SOURCE OUTPUT [--prepared] [--sync] [--profile] [--host-profile] [--buffer bytes] [--rounds count]\n"
          "       session bin://iobench.pxe pipe [--buffer bytes] [--rounds count]\n"
          "buffer: 1..65536 (default read=4088, write/copy=4080, pipe=4096); rounds: 1..100 (default 5)\n"
          "read --bytes: 1..1048576 (default 1048576); exact fixture prefix and EOF required\n"
          "OUTPUT must be a new file; it is retained even on failure\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t clock = startup_resource("clock");
  if (clock == HANDLE_INVALID) {
    fputs("iobench: missing clock grant\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t profile = options.host_profile ? startup_resource("profile") : HANDLE_INVALID;
  if (options.host_profile && profile == HANDLE_INVALID) {
    fputs("iobench: missing requested profile grant\n", stderr);
    return EXIT_FAILURE;
  }
  unsigned char *bytes = options.mode == IO_PIPE ? NULL : malloc(options.bytes);
  if (options.mode != IO_PIPE && !bytes) {
    fputs("iobench: cannot allocate result buffer\n", stderr);
    return EXIT_FAILURE;
  }
  uint64_t times[MAX_ROUNDS], open_times[MAX_ROUNDS], complete_times[MAX_ROUNDS], clock_cost;
  bool success = measure_clock(clock, &clock_cost);
  if (success) {
    fprintf(stderr, "iobench %s: source=%s output=%s buffer=%zu rounds=%zu warmup=1 "
        "fixture=%zu bytes\n", argv[1], options.source ? options.source : "generated",
        options.output ? options.output : "none", options.buffer, options.rounds, options.bytes);
    fprintf(stderr, "Clock-call loop: %llu ns/read (%u reads); not subtracted\n",
        (unsigned long long)clock_cost, CLOCK_READS);
    if (options.mode == IO_PIPE) {
      success = run_pipe(&options, clock);
    } else if (options.mode != IO_READ) {
      success = run_output(&options, bytes, clock);
    } else {
      fprintf(stderr, "Host FILE profiling: %s (measured payload reads only)\n",
          options.host_profile ? "on" : "off");
      struct result result;
      success = read_pass(&options, bytes, clock, profile, false, &result);
      report("warmup", 1, options.bytes, &result, success);
      for (size_t i = 0; success && i < options.rounds; ++i) {
        success = read_pass(&options, bytes, clock, profile, true, &result);
        report("sample", i + 1, options.bytes, &result, success);
        times[i] = result.elapsed;
        open_times[i] = result.open_elapsed;
        complete_times[i] = result.complete_elapsed;
      }
      if (success) {
        print_summary("OPEN", open_times, options.rounds, 0);
        print_summary("Payload read", times, options.rounds, options.bytes);
        print_summary("Complete consumption", complete_times, options.rounds, options.bytes);
      }
    }
  }
  if (!success) {
    fputs("iobench: FAILED; no successful-run summary\n", stderr);
  }
  free(bytes);
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
