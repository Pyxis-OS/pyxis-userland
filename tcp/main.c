#include "../common/dns.h"
#include "../common/udp.h"
#include <clock.h>
#include <handle.h>
#include <startup.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <tcp.h>

#define IO_WAIT_NS UINT64_C(10000000000)
#define TRANSFER_BYTES 4096

static bool report_status(const char *operation, enum call_status status)
{
  const char *reason;
  switch (status) {
  case CALL_TIMED_OUT: reason = "Timed out"; break;
  case CALL_CONNECTION_REFUSED: reason = "Connection refused"; break;
  case CALL_CONNECTION_RESET: reason = "Connection reset"; break;
  case CALL_ENDPOINT_CLOSED: reason = "Connection closed"; break;
  case CALL_NO_ROUTE: reason = "No route to destination"; break;
  case CALL_UNAVAILABLE: reason = "Networking unavailable"; break;
  case CALL_DENIED: reason = "Permission denied"; break;
  case CALL_NO_MEMORY: reason = "Out of network memory"; break;
  case CALL_QUEUE_FULL: reason = "Network queue full"; break;
  case CALL_LIMIT: reason = "Network resource limit reached"; break;
  default: reason = "Operation failed"; break;
  }
  fprintf(stderr, "tcp: %s: %s (status %u)\n", operation, reason, (unsigned)status);
  return false;
}

static bool next_deadline(handle_t clock, uint64_t *deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return report_status("clock", status);
  }
  if (now > UINT64_MAX - IO_WAIT_NS) {
    fputs("tcp: cannot represent deadline\n", stderr);
    return false;
  }
  *deadline = now + IO_WAIT_NS;
  return true;
}

static bool send_request(handle_t stream, handle_t clock, FILE *request)
{
  unsigned char buffer[TRANSFER_BYTES];
  for (;;) {
    size_t count = fread(buffer, 1, sizeof(buffer), request);
    if (ferror(request)) {
      perror("tcp: read request");
      return false;
    }
    if (!count) {
      return true;
    }

    size_t sent = 0;
    while (sent < count) {
      uint64_t deadline;
      if (!next_deadline(clock, &deadline)) {
        return false;
      }
      struct tcp_write_reply reply;
      enum call_status status = tcp_write(stream, buffer + sent, count - sent,
          deadline, &reply);
      if (status != CALL_OK) {
        return report_status("write", status);
      }
      sent += reply.length;
    }
  }
}

static bool receive_response(handle_t stream, handle_t clock)
{
  unsigned char buffer[TRANSFER_BYTES];
  for (;;) {
    uint64_t deadline;
    if (!next_deadline(clock, &deadline)) {
      return false;
    }
    struct tcp_read_reply reply;
    enum call_status status = tcp_read(stream, buffer, sizeof(buffer), deadline, &reply);
    if (status != CALL_OK) {
      return report_status("read", status);
    }
    if (!reply.length) {
      return true;
    }
    if (fwrite(buffer, 1, reply.length, stdout) != reply.length) {
      perror("tcp: stdout");
      return false;
    }
  }
}

int main(int argc, char **argv)
{
  unsigned port;
  if ((argc != 3 && argc != 4) ||
      !udp_parse_number(argv[2], UINT16_MAX, &port) || !port) {
    fputs("Usage: tcp HOST PORT [REQUEST_FILE]\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t service = startup_resource("tcp"), clock = startup_resource("clock");
  if (service == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("tcp: missing tcp or clock capability\n", stderr);
    return EXIT_FAILURE;
  }

  uint32_t address;
  if (!udp_parse_address(argv[1], &address) &&
      !dns_resolve_address("tcp", argv[1], clock, &address)) {
    return EXIT_FAILURE;
  }
  FILE *request = NULL;
  if (argc == 4) {
    request = fopen(argv[3], "rb");
    if (!request) {
      perror("tcp: open request");
      return EXIT_FAILURE;
    }
  }

  bool success = false;
  handle_t stream = HANDLE_INVALID;
  uint64_t deadline;
  if (!next_deadline(clock, &deadline)) {
    goto done;
  }
  struct tcp_connect_reply connection;
  enum call_status status = tcp_connect(service, address, port, deadline, &connection);
  if (status != CALL_OK) {
    report_status("connect", status);
    goto done;
  }
  stream = connection.handle;
  if (request) {
    if (!send_request(stream, clock, request)) {
      goto done;
    }
    int result = fclose(request);
    request = NULL;
    if (result != 0) {
      perror("tcp: close request");
      goto done;
    }
  }

  /* Finish sending before reading: this tool is for finite request/response
   * exchanges, not protocols that require concurrent bidirectional progress. */
  status = tcp_shutdown_write(stream);
  if (status != CALL_OK) {
    report_status("shutdown write", status);
    goto done;
  }
  success = receive_response(stream, clock);

done:
  if (request && fclose(request) != 0) {
    perror("tcp: close request");
    success = false;
  }
  if (stream != HANDLE_INVALID) {
    if (!success) {
      status = tcp_abort(stream);
      if (status != CALL_OK) {
        report_status("abort", status);
      }
    }
    if (handle_close(stream) != 0) {
      fputs("tcp: cannot close stream\n", stderr);
      success = false;
    }
  }
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
