#include "../common/udp.h"
#include <handle.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXCHANGE_WAIT_NS UINT64_C(3000000000)

static void print_reply(const struct udp_receive_reply *reply, const uint8_t *bytes)
{
  uint32_t address = reply->address;
  printf("%llu bytes from %u.%u.%u.%u:%u: \"", (unsigned long long)reply->length,
      address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255,
      (unsigned)reply->port);
  for (size_t i = 0; i < reply->length; ++i) {
    unsigned byte = bytes[i];
    if (byte == '\\' || byte == '"') {
      putchar('\\');
      putchar(byte);
    } else if (byte >= ' ' && byte <= '~') {
      putchar(byte);
    } else {
      printf("\\x%02x", byte);
    }
  }
  puts("\"");
}

int main(int argc, char **argv)
{
  uint32_t local, destination;
  unsigned port;
  if (argc != 5 || !udp_parse_address(argv[1], &local) ||
      !udp_parse_address(argv[2], &destination) ||
      !udp_parse_number(argv[3], UINT16_MAX, &port) || !port) {
    fputs("Usage: udp-send LOCAL_IP DEST_IP PORT MESSAGE\n", stderr);
    return EXIT_FAILURE;
  }
  size_t length = strlen(argv[4]);
  if (length > UDP_MAX_PAYLOAD) {
    fprintf(stderr, "udp-send: message exceeds %u bytes\n", (unsigned)UDP_MAX_PAYLOAD);
    return EXIT_FAILURE;
  }
  handle_t service = startup_resource("udp"), clock = startup_resource("clock");
  if (service == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("udp-send: missing udp or clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  struct udp_open_reply endpoint;
  enum call_status status = udp_open(service, local, 0, &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "udp-send: open: %s (status %u)\n", udp_error(status), (unsigned)status);
    return EXIT_FAILURE;
  }

  uint64_t deadline;
  status = udp_deadline(clock, EXCHANGE_WAIT_NS, &deadline);
  if (status == CALL_OK) {
    status = udp_send(endpoint.handle, destination, port, argv[4], length, deadline);
  }
  if (status == CALL_OK) {
    status = udp_deadline(clock, EXCHANGE_WAIT_NS, &deadline);
  }
  while (status == CALL_OK) {
    uint8_t bytes[UDP_MAX_PAYLOAD];
    struct udp_receive_reply reply;
    status = udp_receive(endpoint.handle, bytes, sizeof(bytes), deadline, &reply);
    if (status != CALL_OK) {
      break;
    }
    /* Ignore other peers without extending the original receive deadline. */
    if (reply.address != destination || reply.port != port) {
      continue;
    }
    print_reply(&reply, bytes);
    break;
  }
  int result = EXIT_SUCCESS;
  if (status != CALL_OK) {
    fprintf(stderr, "udp-send: %s (status %u)\n", udp_error(status), (unsigned)status);
    result = EXIT_FAILURE;
  }
  if (handle_close(endpoint.handle) != 0) {
    fputs("udp-send: cannot close endpoint\n", stderr);
    result = EXIT_FAILURE;
  }
  if (ferror(stdout) || ferror(stderr)) {
    result = EXIT_FAILURE;
  }
  return result;
}
