#include "../common/udp.h"
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ECHO_IDLE_NS UINT64_C(30000000000)
#define ECHO_SEND_NS UINT64_C(3000000000)
#define ECHO_MAX_COUNT 65535

int main(int argc, char **argv)
{
  uint32_t local;
  unsigned port, count = 0;
  if ((argc != 3 && argc != 5) || !udp_parse_address(argv[1], &local) ||
      !udp_parse_number(argv[2], UINT16_MAX, &port) || !port ||
      (argc == 5 && (strcmp(argv[3], "--count") ||
       !udp_parse_number(argv[4], ECHO_MAX_COUNT, &count) || !count))) {
    fputs("Usage: udp-echo LOCAL_IP PORT [--count N]\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t service = startup_resource("udp"), clock = startup_resource("clock");
  if (service == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("udp-echo: missing udp or clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  struct udp_open_reply endpoint;
  enum call_status status = udp_open(service, local, port, &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "udp-echo: open: %s (status %u)\n", udp_error(status), (unsigned)status);
    return EXIT_FAILURE;
  }
  int result = EXIT_SUCCESS;
  if (printf("udp-echo: listening on %s:%u; exits after 30 idle seconds\n", argv[1], port) < 0 ||
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
      status = udp_send(endpoint.handle, reply.address, reply.port, bytes, reply.length, deadline);
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
}
