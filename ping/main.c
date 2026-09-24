#include <clock.h>
#include <echo.h>
#include <startup.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PING_DEFAULT_COUNT 4
#define PING_MAX_COUNT 65535
#define PING_INTERVAL_NS UINT64_C(1000000000)
#define PING_TIMEOUT_NS UINT64_C(1000000000)

/* Numeric dotted decimal only; no signs, shorthand, octal or DNS. */
static bool parse_address(const char *text, uint32_t *address)
{
  uint32_t result = 0;
  for (unsigned i = 0; i < 4; ++i) {
    unsigned value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') {
      if (++digits > 3) {
        return false;
      }
      value = value * 10 + (unsigned)(*text++ - '0');
    }
    if (!digits || value > 255 || (i != 3 && *text++ != '.')) {
      return false;
    }
    result = (result << 8) | value;
  }
  if (*text) {
    return false;
  }
  *address = result;
  return true;
}

static bool parse_count(const char *text, unsigned *count)
{
  unsigned value = 0;
  if (!*text) {
    return false;
  }
  while (*text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    unsigned digit = (unsigned)(*text++ - '0');
    if (value > (PING_MAX_COUNT - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *count = value;
  return value != 0;
}

static const char *echo_error(enum call_status status)
{
  switch (status) {
  case CALL_TIMED_OUT: return "Timed out";
  case CALL_NO_ROUTE: return "No route to destination";
  case CALL_QUEUE_FULL: return "Echo queue full";
  case CALL_DENIED: return "Echo permission denied";
  case CALL_NO_MEMORY: return "Out of packet memory";
  case CALL_UNAVAILABLE: return "Networking unavailable";
  default: return "Echo request failed";
  }
}

int main(int argc, char **argv)
{
  unsigned count = PING_DEFAULT_COUNT;
  const char *target;
  if (argc == 2) {
    target = argv[1];
  } else if (argc == 4 && !strcmp(argv[1], "-c") && parse_count(argv[2], &count)) {
    target = argv[3];
  } else {
    fputs("Usage: ping [-c count] IPv4-address\n", stderr);
    return EXIT_FAILURE;
  }
  uint32_t address;
  if (!parse_address(target, &address)) {
    fprintf(stderr, "ping: invalid numeric IPv4 address: %s\n", target);
    return EXIT_FAILURE;
  }
  handle_t echo = startup_resource("echo"), clock = startup_resource("clock");
  if (echo == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("ping: missing echo or clock capability\n", stderr);
    return EXIT_FAILURE;
  }

  if (printf("PING %s: %u data bytes\n", target, ECHO_PAYLOAD_BYTES) < 0) {
    perror("ping: output");
    return EXIT_FAILURE;
  }
  unsigned attempted = 0, received = 0;
  uint64_t minimum = UINT64_MAX, maximum = 0, total = 0;
  bool failed = false;
  for (unsigned i = 0; i < count; ++i) {
    uint64_t started;
    enum call_status status = clock_now(clock, &started);
    if (status != CALL_OK || started > UINT64_MAX - PING_TIMEOUT_NS) {
      fputs("ping: cannot read clock or represent deadline\n", stderr);
      failed = true;
      break;
    }
    struct echo_reply reply;
    ++attempted;
    status = echo_exchange(echo, address, started + PING_TIMEOUT_NS, &reply);
    if (status == CALL_OK) {
      ++received;
      if (reply.round_trip_ns < minimum) {
        minimum = reply.round_trip_ns;
      }
      if (reply.round_trip_ns > maximum) {
        maximum = reply.round_trip_ns;
      }
      total += reply.round_trip_ns;
      printf("%u bytes from %s: seq=%u time=%.3f ms\n",
          ECHO_PAYLOAD_BYTES, target, (unsigned)reply.sequence,
          (double)reply.round_trip_ns / 1000000.0);
    } else {
      printf("request %u: %s (status %u)\n", i + 1, echo_error(status), (unsigned)status);
      failed = true;
      if (status != CALL_TIMED_OUT) {
        break;
      }
    }
    if (ferror(stdout)) {
      perror("ping: output");
      return EXIT_FAILURE;
    }
    if (i + 1 < count && clock_sleep_until(clock, started + PING_INTERVAL_NS) != CALL_OK) {
      fputs("ping: cannot wait for next request\n", stderr);
      failed = true;
      break;
    }
  }
  printf("%u attempted, %u replies, %u unanswered\n",
      attempted, received, attempted - received);
  if (received) {
    printf("round-trip min/avg/max = %.3f/%.3f/%.3f ms\n",
        (double)minimum / 1000000.0, (double)total / received / 1000000.0,
        (double)maximum / 1000000.0);
  }
  if (ferror(stdout)) {
    perror("ping: output");
    return EXIT_FAILURE;
  }
  return !failed && received == count ? EXIT_SUCCESS : EXIT_FAILURE;
}
