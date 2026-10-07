#include <clock.h>
#include <log.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FOLLOW_INTERVAL_NS UINT64_C(100000000)

static int report_status(const char *operation, enum call_status status)
{
  fprintf(stderr, "log: %s (status %u)\n", operation, status);
  return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  bool follow = argc == 2 && !strcmp(argv[1], "-f");
  if (argc != 1 && !follow) {
    fputs("usage: log [-f]\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t log = startup_resource("log"), clock = startup_resource("clock");
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
  unsigned char bytes[LOG_READ_MAX];
  while (true) {
    struct log_read_reply reply;
    status = log_read(log, cursor, end, bytes, sizeof(bytes), &reply);
    if (status != CALL_OK) {
      return report_status("cannot read", status);
    }
    cursor = reply.next;
    if (reply.dropped_lines && fprintf(stderr, "log: lost %llu lines\n",
        (unsigned long long)reply.dropped_lines) < 0) {
      return EXIT_FAILURE;
    }
    if (reply.size && fwrite(bytes, 1, reply.size, stdout) != reply.size) {
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
