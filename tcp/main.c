#include "../common/dns.h"
#include "../common/udp.h"
#include <clock.h>
#include <handle.h>
#include <limits.h>
#include <startup.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static bool send_buffer(handle_t stream, handle_t clock, const unsigned char *buffer,
    size_t count)
{
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

    if (!send_buffer(stream, clock, buffer, count)) {
      return false;
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

static bool echo_connection(handle_t stream, handle_t clock)
{
  unsigned char buffer[TRANSFER_BYTES];
  for (;;) {
    uint64_t deadline;
    if (!next_deadline(clock, &deadline)) {
      return false;
    }
    struct tcp_read_reply reply;
    enum call_status status = tcp_read(stream, buffer, sizeof(buffer), deadline, &reply);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return report_status("read", status);
    }
    if (!reply.length) {
      status = tcp_shutdown_write(stream);
      return status == CALL_OK || report_status("shutdown write", status);
    }
    if (!send_buffer(stream, clock, buffer, reply.length)) {
      return false;
    }
  }
}

static int serve(unsigned count)
{
  handle_t listener = startup_resource("tcp_listener"), clock = startup_resource("clock");
  if (listener == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("tcp: missing tcp_listener or clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  struct tcp_listener_info info;
  enum call_status status = tcp_listener_inspect(listener, &info);
  bool success = false;
  if (status != CALL_OK) {
    report_status("inspect listener", status);
    goto done;
  }
  int printed = printf("tcp: listening on %u.%u.%u.%u:%u\n", info.local_address >> 24,
      (info.local_address >> 16) & 255, (info.local_address >> 8) & 255,
      info.local_address & 255, info.local_port);
  if (printed < 0) {
    perror("tcp: stdout");
    goto done;
  }

  unsigned served = 0;
  while (!count || served < count) {
    uint64_t deadline;
    if (!next_deadline(clock, &deadline)) {
      goto done;
    }
    struct tcp_accept_reply connection;
    status = tcp_accept(listener, deadline, &connection);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      report_status("accept", status);
      goto done;
    }
    bool admitted = true;
    if (count && served == count - 1) {
      /* The final accepted stream is independent of the listener. Stop
       * admission now, including aborting excess queued connections. */
      if (handle_close(listener) != 0) {
        fputs("tcp: cannot close listener\n", stderr);
        admitted = false;
      } else {
        listener = HANDLE_INVALID;
      }
    }
    printed = printf("tcp: accepted %u.%u.%u.%u:%u\n", connection.connection.remote_address >> 24,
        (connection.connection.remote_address >> 16) & 255,
        (connection.connection.remote_address >> 8) & 255,
        connection.connection.remote_address & 255, connection.connection.remote_port);
    bool echoed = admitted && printed >= 0 && echo_connection(connection.handle, clock);
    if (!echoed) {
      status = tcp_abort(connection.handle);
      if (status != CALL_OK) {
        report_status("abort", status);
      }
    }
    if (handle_close(connection.handle) != 0) {
      fputs("tcp: cannot close stream\n", stderr);
      echoed = false;
    }
    if (!echoed) {
      goto done;
    }
    ++served;
  }
  success = true;

done:
  if (listener != HANDLE_INVALID && handle_close(listener) != 0) {
    fputs("tcp: cannot close listener\n", stderr);
    success = false;
  }
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  if (argc >= 2 && !strcmp(argv[1], "--serve")) {
    unsigned count = 0;
    if ((argc != 2 && argc != 3) ||
        (argc == 3 && (!udp_parse_number(argv[2], UINT_MAX, &count) || !count))) {
      fputs("Usage: tcp --serve [COUNT]\n", stderr);
      return EXIT_FAILURE;
    }
    return serve(count);
  }
  unsigned port;
  if ((argc != 3 && argc != 4) ||
      !udp_parse_number(argv[2], UINT16_MAX, &port) || !port) {
    fputs("Usage: tcp HOST PORT [REQUEST_FILE]\n       tcp --serve [COUNT]\n", stderr);
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
