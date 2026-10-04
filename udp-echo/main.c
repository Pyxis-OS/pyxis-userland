#include "../common/udp.h"
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ECHO_IDLE_NS UINT64_C(30000000000)
#define ECHO_SEND_NS UINT64_C(3000000000)
#define ECHO_MAX_COUNT 65535
#define LIMITED_BROADCAST UINT32_MAX

int main(int argc, char **argv)
{
  bool inherited = argc >= 2 && !strcmp(argv[1], "--endpoint");
  int first_option = inherited ? 2 : 3;
  uint32_t local = 0;
  unsigned port = 0, count = 0;
  if (argc != first_option && argc != first_option + 2) {
    goto usage;
  }
  if (!inherited && (!udp_parse_address(argv[1], &local) ||
      !udp_parse_number(argv[2], UINT16_MAX, &port) || !port)) {
    goto usage;
  }
  if (argc == first_option + 2 && (strcmp(argv[first_option], "--count") ||
      !udp_parse_number(argv[first_option + 1], ECHO_MAX_COUNT, &count) || !count)) {
    goto usage;
  }
  handle_t clock = startup_resource("clock");
  if (clock == HANDLE_INVALID) {
    fputs("udp-echo: missing clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  struct udp_open_reply endpoint = {.handle = HANDLE_INVALID};
  enum call_status status;
  if (inherited) {
    endpoint.handle = startup_resource("udp_endpoint");
    status = udp_inspect(endpoint.handle, &endpoint.local);
    if (status == CALL_OK && (endpoint.local.address ||
        endpoint.local.state != UDP_STATE_BOUND)) {
      status = CALL_BAD_REQUEST;
    }
    port = endpoint.local.port;
  } else {
    handle_t service = startup_resource("udp");
    status = udp_open(service, local, port, &endpoint);
  }
  if (status != CALL_OK) {
    fprintf(stderr, "udp-echo: open: %s (status %u)\n", udp_error(status), (unsigned)status);
    if (endpoint.handle != HANDLE_INVALID) {
      handle_close(endpoint.handle);
    }
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  const char *address = inherited ? "net0 wildcard" : argv[1];
  if (printf("udp-echo: listening on %s:%u; exits after 30 idle seconds\n", address, port) < 0 ||
      fflush(stdout) == EOF) {
    result = EXIT_FAILURE;
    goto close;
  }

  unsigned echoed = 0;
  for (;;) {
    uint64_t deadline;
    status = udp_deadline(clock, ECHO_IDLE_NS, &deadline);
    if (status != CALL_OK) {
      break;
    }
    uint8_t bytes[UDP_MAX_PAYLOAD];
    struct udp_receive_reply reply;
    status = udp_receive(endpoint.handle, bytes, sizeof(bytes), deadline, &reply);
    if (status == CALL_TIMED_OUT) {
      puts("udp-echo: idle timeout");
      status = CALL_OK;
      break;
    }
    if (status != CALL_OK) {
      break;
    }
    /* IPv4 UDP permits absent source ports; there is no valid reply target. */
    if (!reply.port) {
      continue;
    }
    status = udp_deadline(clock, ECHO_SEND_NS, &deadline);
    if (status == CALL_OK) {
      /* The trusted wildcard mode echoes by limited broadcast, including
       * before assignment. Ordinary echo remains a unicast reply. */
      uint32_t destination = inherited ? LIMITED_BROADCAST : reply.address;
      status = udp_send(endpoint.handle, destination, reply.port, bytes, reply.length, deadline);
    }
    if (status != CALL_OK) {
      break;
    }
    if (count && ++echoed == count) {
      break;
    }
  }
  if (status != CALL_OK) {
    fprintf(stderr, "udp-echo: %s (status %u)\n", udp_error(status), (unsigned)status);
    result = EXIT_FAILURE;
  }

close:
  if (handle_close(endpoint.handle) != 0) {
    fputs("udp-echo: cannot close endpoint\n", stderr);
    result = EXIT_FAILURE;
  }
  if (ferror(stdout) || ferror(stderr)) {
    result = EXIT_FAILURE;
  }
  return result;

usage:
  fputs("Usage: udp-echo LOCAL_IP PORT [--count N]\n"
      "       udp-echo --endpoint [--count N] (trusted broadcast handoff)\n", stderr);
  return EXIT_FAILURE;
}
