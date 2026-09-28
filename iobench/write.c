#include "iobench.h"
#include "../common/directory.h"
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct output_result {
  size_t read_bytes, written_bytes, read_calls, write_calls;
  size_t short_reads, short_writes;
  uint64_t elapsed, sync_elapsed;
  bool timed, sync_timed, early_eof;
  const char *operation;
};

static bool check_status(const char *operation, enum call_status status)
{
  if (status == CALL_OK) {
    return true;
  }
  report_directory_error("iobench", operation, status);
  if (status == CALL_OUTCOME_UNKNOWN) {
    fputs("iobench: mutation not retried; output may exceed confirmed progress\n", stderr);
  }
  return false;
}

static enum call_status create_output(const char *path, handle_t *output)
{
  *output = HANDLE_INVALID;
  if (!*path || *path == '/') {
    return CALL_BAD_REQUEST;
  }
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) {
    return CALL_BAD_REQUEST;
  }
  char *parent = slash ? strndup(path, name - path) : strdup(".");
  if (!parent) {
    return CALL_NO_MEMORY;
  }
  handle_t directory = HANDLE_INVALID;
  uint64_t rights = DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_CREATE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES;
  enum call_status status = resolve_directory(parent, rights, &directory);
  free(parent);
  if (status == CALL_OK) {
    /* Never use open-or-create: this operation must reject existing names. */
    status = directory_create(directory, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, output);
  }
  if (directory != HANDLE_INVALID && handle_close(directory) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  return status;
}

static bool check_eof(handle_t file)
{
  unsigned char extra;
  size_t count;
  if (!check_status("EOF check", file_read(file, FIXTURE_BYTES, &extra, 1, &count))) {
    return false;
  }
  if (count) {
    fputs("iobench: fixture exceeds 1048576 bytes\n", stderr);
    return false;
  }
  return true;
}

static bool verify_file(handle_t file, unsigned char *scratch)
{
  fill_fixture(scratch, FIXTURE_BYTES, true);
  size_t offset = 0;
  while (offset < FIXTURE_BYTES) {
    size_t count;
    if (!check_status("verification read", file_read(file, offset, scratch + offset,
          FIXTURE_BYTES - offset, &count))) {
      return false;
    }
    if (!count) {
      fprintf(stderr, "iobench: verification reached EOF after %zu bytes\n", offset);
      return false;
    }
    offset += count;
  }
  return check_eof(file) && verify_fixture(scratch, FIXTURE_BYTES);
}

static bool prepare_output(const struct options *options, handle_t output,
    unsigned char *scratch)
{
  if (options->prepared) {
    fill_fixture(scratch, FIXTURE_BYTES, true);
    size_t offset = 0;
    while (offset < FIXTURE_BYTES) {
      size_t count;
      if (!check_status("preparation write", file_write(output, offset, scratch + offset,
            FIXTURE_BYTES - offset, &count))) {
        fprintf(stderr, "iobench: preparation confirmed %zu bytes\n", offset);
        return false;
      }
      offset += count;
    }
  } else if (!check_status("reset output", file_resize(output, 0))) {
    return false;
  }
  /* Keep preparation's dirty state out of the measured sync boundary. */
  return !options->sync || check_status("preparation sync", file_sync(output));
}

static enum call_status transfer(const struct options *options, handle_t source,
    handle_t output, unsigned char *payload, struct output_result *result)
{
  while (result->written_bytes < FIXTURE_BYTES) {
    size_t request = FIXTURE_BYTES - result->written_bytes;
    if (request > options->buffer) {
      request = options->buffer;
    }
    if (options->mode == IO_COPY) {
      if (result->read_bytes == result->written_bytes) {
        size_t count;
        result->operation = "source read";
        ++result->read_calls;
        enum call_status status = file_read(source, result->read_bytes,
            payload + result->read_bytes, request, &count);
        if (status != CALL_OK) {
          return status;
        }
        if (!count) {
          result->early_eof = true;
          return CALL_BAD_REQUEST;
        }
        if (count < request) {
          ++result->short_reads;
        }
        result->read_bytes += count;
      }
      /* Drain this read before issuing another, including positive short writes. */
      request = result->read_bytes - result->written_bytes;
    }
    size_t count;
    result->operation = "output write";
    ++result->write_calls;
    enum call_status status = file_write(output, result->written_bytes,
        payload + result->written_bytes, request, &count);
    if (status != CALL_OK) {
      return status;
    }
    if (count < request) {
      ++result->short_writes;
    }
    result->written_bytes += count;
  }
  return CALL_OK;
}

static bool output_pass(const struct options *options, handle_t source, handle_t output,
    unsigned char *payload, unsigned char *scratch, handle_t clock, bool timed,
    struct output_result *result)
{
  *result = (struct output_result){0};
  if (!prepare_output(options, output, scratch)) {
    return false;
  }
  if (options->mode == IO_COPY) {
    fill_fixture(payload, FIXTURE_BYTES, true);
  }

  uint64_t start = 0, end = 0, sync_start = 0, sync_end = 0;
  if (timed && !check_status("transfer start clock", clock_now(clock, &start))) {
    return false;
  }
  enum call_status status = transfer(options, source, output, payload, result);
  enum call_status end_status = timed ? clock_now(clock, &end) : CALL_OK;
  enum call_status sync_status = CALL_OK, sync_clock_status = CALL_OK;
  if (status == CALL_OK && end_status == CALL_OK && options->sync) {
    sync_clock_status = timed ? clock_now(clock, &sync_start) : CALL_OK;
    if (sync_clock_status == CALL_OK) {
      sync_status = file_sync(output);
      if (timed) {
        sync_clock_status = clock_now(clock, &sync_end);
      }
    }
  }

  /* All diagnostics and verification follow both measured intervals. */
  bool success = check_status(result->operation, status);
  if (result->early_eof) {
    fprintf(stderr, "iobench: source ended after %zu bytes\n", result->read_bytes);
  }
  if (!check_status("transfer end clock", end_status)) {
    success = false;
  } else if (timed) {
    if (end > start) {
      result->elapsed = end - start;
      result->timed = true;
    } else {
      fputs("iobench: transfer clock did not advance\n", stderr);
      success = false;
    }
  }
  if (!check_status("output sync", sync_status)) {
    success = false;
  }
  if (!check_status("sync clock", sync_clock_status)) {
    success = false;
  }
  if (timed && options->sync && status == CALL_OK && end_status == CALL_OK &&
      sync_clock_status == CALL_OK) {
    if (sync_end > sync_start) {
      result->sync_elapsed = sync_end - sync_start;
      result->sync_timed = true;
    } else {
      fputs("iobench: sync clock did not advance\n", stderr);
      success = false;
    }
  }
  if (success && options->mode == IO_COPY) {
    success = check_eof(source);
  }
  if (success && !verify_file(output, scratch)) {
    fputs("iobench: output verification failed\n", stderr);
    success = false;
  }
  return success;
}

static void report_pass(size_t pass, const struct output_result *result, bool success)
{
  fprintf(stderr, "%s %zu: %s; requested=%u read=%zu written=%zu bytes; failed_pass=%u\n",
      pass ? "sample" : "warmup", pass ? pass : 1, success ? "OK" : "FAILED",
      FIXTURE_BYTES, result->read_bytes, result->written_bytes, success ? 0U : 1U);
  fprintf(stderr, "  file_read_calls=%zu short_reads=%zu file_write_calls=%zu short_writes=%zu\n",
      result->read_calls, result->short_reads, result->write_calls, result->short_writes);
  if (result->timed) {
    fprintf(stderr, "  transfer=%llu ns (%.3f ms)",
        (unsigned long long)result->elapsed, result->elapsed / 1000000.0);
    if (success) {
      fprintf(stderr, "; %.3f MiB/s", result->written_bytes * 1000000000.0 /
          result->elapsed / (1024.0 * 1024.0));
    }
    fputc('\n', stderr);
  }
  if (result->sync_timed) {
    fprintf(stderr, "  file_sync=%llu ns (%.3f ms)\n",
        (unsigned long long)result->sync_elapsed, result->sync_elapsed / 1000000.0);
  }
}

bool run_output(const struct options *options, unsigned char *scratch, handle_t clock)
{
  unsigned char *payload = malloc(FIXTURE_BYTES);
  if (!payload) {
    fputs("iobench: cannot allocate 1 MiB payload buffer\n", stderr);
    return false;
  }
  handle_t source = HANDLE_INVALID, output = HANDLE_INVALID;
  bool success = true;
  fprintf(stderr, "Interface: libpyxis file handles; storage=%s sync=%s\n",
      options->prepared ? "prepared overwrite" : "grow from zero",
      options->sync ? "file (preparation untimed, completion separate)" : "none");
  if (options->mode == IO_COPY) {
    success = check_status("open source", resolve_file(options->source, FILE_RIGHT_READ, &source));
    if (success && !verify_file(source, scratch)) {
      fputs("iobench: source verification failed before output creation\n", stderr);
      success = false;
    }
  } else {
    fill_fixture(payload, FIXTURE_BYTES, false);
  }
  if (success) {
    success = check_status("exclusive output creation", create_output(options->output, &output));
    if (!success) {
      fputs("iobench: no creation retry or removal; inspect destination after host errors\n", stderr);
    }
  }
  uint64_t times[MAX_ROUNDS], sync_times[MAX_ROUNDS];
  for (size_t i = 0; success && i <= options->rounds; ++i) {
    struct output_result result;
    success = output_pass(options, source, output, payload, scratch, clock, i != 0, &result);
    report_pass(i, &result, success);
    if (i) {
      times[i - 1] = result.elapsed;
      sync_times[i - 1] = result.sync_elapsed;
    }
  }
  if (source != HANDLE_INVALID && handle_close(source) != 0) {
    fputs("iobench: source close failed; not retried\n", stderr);
    success = false;
  }
  if (output != HANDLE_INVALID) {
    if (handle_close(output) != 0) {
      fputs("iobench: output close failed; not retried\n", stderr);
      success = false;
    }
    fprintf(stderr, "Output retained: %s%s\n", options->output,
        success ? "" : " (run failed; contents may be partial)");
  }
  free(payload);
  if (success) {
    print_summary("Transfer", times, options->rounds, FIXTURE_BYTES);
    if (options->sync) {
      print_summary("File sync", sync_times, options->rounds, 0);
    }
  }
  return success;
}
