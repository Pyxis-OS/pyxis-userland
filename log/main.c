#include "../common/bootstrap_environment.h"
#include <clock.h>
#include <log.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLLOW_INTERVAL_NS UINT64_C(100000000)

static const char *const level_names[] = {
  [LOG_LEVEL_ERROR] = "error",
  [LOG_LEVEL_WARN] = "warn",
  [LOG_LEVEL_INFO] = "info",
  [LOG_LEVEL_DEBUG] = "debug",
  [LOG_LEVEL_TRACE] = "trace",
};

static const char level_letters[] = {
  [LOG_LEVEL_ERROR] = 'E',
  [LOG_LEVEL_WARN] = 'W',
  [LOG_LEVEL_INFO] = 'I',
  [LOG_LEVEL_DEBUG] = 'D',
  [LOG_LEVEL_TRACE] = 'T',
};

static int report_status(const char *operation, enum call_status status)
{
  fprintf(stderr, "log: %s (status %u)\n", operation, status);
  return EXIT_FAILURE;
}

static int usage(void)
{
  fputs("usage: log [-f] [-l error|warn|info|debug|trace] [-s NAME]\n", stderr);
  return EXIT_FAILURE;
}

static bool parse_level(const char *name, enum log_level *level)
{
  for (enum log_level candidate = LOG_LEVEL_ERROR; candidate <= LOG_LEVEL_TRACE; ++candidate) {
    if (!strcmp(name, level_names[candidate])) {
      *level = candidate;
      return true;
    }
  }
  return false;
}

/* Prints the level column at the start of each shown line. A piece that
 * continues a line keeps that line's decision, so filtering never splits one.
 * With a source, only lines written by that space show: their text starts with
 * its name and ": ", stamped by the kernel. */
static bool print_records(const unsigned char *bytes, size_t size, enum log_level shown,
    const char *source, bool *line_start, bool *line_shown)
{
  size_t offset = 0;
  struct log_record record;
  const char *text;
  size_t source_length = source ? strlen(source) : 0;
  while (log_next_record(bytes, size, &offset, &record, &text)) {
    bool starts = *line_start;
    *line_start = text[record.length - 1] == '\n';
    if (starts) {
      *line_shown = record.level <= shown;
      if (*line_shown && source) {
        size_t compared = record.length < source_length + 2 ? record.length : source_length + 2;
        char prefix[LOG_WRITE_MAX + 2];
        memcpy(prefix, source, source_length);
        memcpy(prefix + source_length, ": ", 2);
        *line_shown = record.source && !memcmp(text, prefix, compared);
      }
    }
    if (!*line_shown) {
      continue;
    }
    if (starts && printf("%c ", level_letters[record.level]) < 0) {
      return false;
    }
    if (fwrite(text, 1, record.length, stdout) != record.length) {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv)
{
  bool follow = false;
  enum log_level shown = LOG_LEVEL_TRACE;
  const char *source = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-f") && !follow) {
      follow = true;
    } else if (!strcmp(argv[i], "-l") && i + 1 < argc && parse_level(argv[i + 1], &shown)) {
      ++i;
    } else if (!strcmp(argv[i], "-s") && i + 1 < argc && !source &&
        strlen(argv[i + 1]) && strlen(argv[i + 1]) <= LOG_WRITE_MAX) {
      source = argv[++i];
    } else {
      return usage();
    }
  }
  handle_t log = bootstrap_capability("log"), clock = bootstrap_capability("clock");
  if (log == HANDLE_INVALID) {
    fputs("log: missing read grant\n", stderr);
    return EXIT_FAILURE;
  }
  if (follow && clock == HANDLE_INVALID) {
    fputs("log: follow requires a clock grant\n", stderr);
    return EXIT_FAILURE;
  }
  struct log_snapshot snapshot;
  enum call_status status = log_get_snapshot(log, &snapshot);
  if (status != CALL_OK) {
    return report_status("cannot snapshot", status);
  }
  struct log_cursor cursor = {0, 0}, end = snapshot.end;
  bool line_start = true, line_shown = false;
  unsigned char bytes[LOG_READ_MAX];
  while (true) {
    struct log_read_reply reply;
    status = log_read(log, cursor, end, bytes, sizeof(bytes), &reply);
    if (status != CALL_OK) {
      return report_status("cannot read", status);
    }
    cursor = reply.next;
    if (reply.dropped_lines) {
      if (fprintf(stderr, "log: lost %llu lines\n",
          (unsigned long long)reply.dropped_lines) < 0) {
        return EXIT_FAILURE;
      }
      line_start = true;
    }
    if (!print_records(bytes, reply.size, shown, source, &line_start, &line_shown)) {
      perror("log: stdout");
      return EXIT_FAILURE;
    }
    if (reply.size) {
      continue;
    }
    if (!follow) {
      break;
    }
    status = clock_sleep_for(clock, FOLLOW_INTERVAL_NS);
    if (status != CALL_OK) {
      return report_status("cannot wait", status);
    }
    end = (struct log_cursor){UINT64_MAX, UINT64_MAX};
  }
  if (fclose(stdout) != 0) {
    perror("log: stdout");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
