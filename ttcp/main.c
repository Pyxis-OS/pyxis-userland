#include "../common/dns.h"
#include "../common/udp.h"
#include <clock.h>
#include <handle.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>

#define DEFAULT_PORT 5001
#define DEFAULT_BUFFERS 2048
#define DEFAULT_BUFFER_BYTES 8192
#define WAIT_NS UINT64_C(10000000000)
#define CLOSE_POLL_NS UINT64_C(10000000)

struct options {
  const char *host;
  bool transmit;
  unsigned port, buffers, length;
};

static bool parse_options(int argc, char **argv, struct options *options)
{
  *options = (struct options){
    .port = DEFAULT_PORT, .buffers = DEFAULT_BUFFERS, .length = DEFAULT_BUFFER_BYTES,
  };
  bool transmit = false, receive = false, sized = false;
  int i = 1;
  while (i < argc && argv[i][0] == '-') {
    const char *option = argv[i++];
    if (!strcmp(option, "-t")) {
      transmit = true;
      continue;
    }
    if (!strcmp(option, "-r")) {
      receive = true;
      continue;
    }
    if (option[1] != 'p' && option[1] != 'n' && option[1] != 'l') {
      return false;
    }
    sized |= option[1] != 'p';
    const char *number = option + 2;
    if (!*number) {
      if (i == argc) {
        return false;
      }
      number = argv[i++];
    }
    unsigned value;
    unsigned maximum = option[1] == 'p' ? UINT16_MAX : UINT32_MAX;
    if (!udp_parse_number(number, maximum, &value) || !value) {
      return false;
    }
    switch (option[1]) {
    case 'p': options->port = value; break;
    case 'n': options->buffers = value; break;
    case 'l': options->length = value; break;
    }
  }
  /* Receive sizes are the peer's; -n and -l describe only what -t sends. */
  if (transmit == receive || (receive && sized) || i != argc - 1 || !argv[i][0]) {
    return false;
  }
  options->transmit = transmit;
  options->host = argv[i];
  return true;
}

static bool report_status(const char *operation, enum call_status status)
{
  const char *reason = status == CALL_CONNECTION_REFUSED ? "Connection refused" :
      status == CALL_CONNECTION_RESET ? "Connection reset" : udp_error(status);
  fprintf(stderr, "ttcp: %s: %s (status %u)\n", operation, reason, (unsigned)status);
  return false;
}

static bool read_clock(handle_t clock, uint64_t *now)
{
  enum call_status status = clock_now(clock, now);
  return status == CALL_OK || report_status("clock", status);
}

static bool next_deadline(handle_t clock, uint64_t *deadline)
{
  uint64_t now;
  if (!read_clock(clock, &now)) {
    return false;
  }
  if (now > UINT64_MAX - WAIT_NS) {
    fputs("ttcp: cannot represent deadline\n", stderr);
    return false;
  }
  *deadline = now + WAIT_NS;
  return true;
}

/* Classic ttcp's printable ASCII pattern, restarted for each source buffer.
 * This native implementation's provenance is recorded in README.md. */
static void fill_pattern(unsigned char *buffer, size_t length)
{
  unsigned char byte = ' ';
  for (size_t i = 0; i < length; ++i) {
    buffer[i] = byte;
    byte = byte == '~' ? ' ' : byte + 1;
  }
}

static bool transmit(handle_t stream, handle_t clock, const unsigned char *buffer,
    const struct options *options, uint64_t *accepted)
{
  for (unsigned i = 0; i < options->buffers; ++i) {
    size_t offset = 0;
    while (offset < options->length) {
      uint64_t deadline;
      if (!next_deadline(clock, &deadline)) {
        return false;
      }
      struct tcp_write_reply reply;
      enum call_status status = tcp_write(stream, buffer + offset,
          options->length - offset, deadline, &reply);
      if (status != CALL_OK) {
        return report_status("write", status);
      }
      offset += reply.length;
      *accepted += reply.length;
    }
  }
  return true;
}

/* Read and discard until peer EOF. Each read has a fresh deadline, so a peer
 * that stops sending without closing fails after ten seconds. */
static bool receive(handle_t stream, handle_t clock, uint64_t *received, uint64_t *finished)
{
  unsigned char buffer[TCP_READ_MAX_BYTES];
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
      return read_clock(clock, finished);
    }
    *received += reply.length;
  }
}

static bool finish_transfer(handle_t stream, handle_t clock, uint64_t *finished)
{
  uint64_t deadline;
  if (!next_deadline(clock, &deadline)) {
    return false;
  }
  enum call_status status = tcp_shutdown_write(stream);
  if (status != CALL_OK) {
    return report_status("shutdown write", status);
  }

  /* One deadline covers draining peer data, EOF and the final FIN ACK. A peer
   * that keeps sending must not extend the completion wait indefinitely. */
  unsigned char buffer[TCP_READ_MAX_BYTES];
  for (;;) {
    struct tcp_read_reply reply;
    status = tcp_read(stream, buffer, sizeof(buffer), deadline, &reply);
    if (status != CALL_OK) {
      return report_status("wait for peer EOF", status);
    }
    if (!reply.length) {
      break;
    }
  }

  /* EOF alone can arrive before our data/FIN is acknowledged. CLOSED with
   * no terminal error and both shutdown flags confirms orderly transport
   * closure, including TIME_WAIT; it does not prove application consumption. */
  for (;;) {
    struct tcp_connection_info info;
    status = tcp_inspect(stream, &info);
    if (status != CALL_OK || info.terminal_status != CALL_OK) {
      return report_status("wait for closure", status != CALL_OK ? status : info.terminal_status);
    }
    uint64_t now;
    if (!read_clock(clock, &now)) {
      return false;
    }
    if (now >= deadline) {
      return report_status("wait for closure", CALL_TIMED_OUT);
    }
    uint32_t closed_flags = TCP_INFO_WRITE_SHUTDOWN | TCP_INFO_PEER_FIN;
    if (info.state == TCP_STATE_CLOSED && (info.flags & closed_flags) == closed_flags) {
      *finished = now;
      return true;
    }
    uint64_t wait = deadline - now < CLOSE_POLL_NS ? deadline - now : CLOSE_POLL_NS;
    status = clock_sleep_until(clock, now + wait);
    if (status != CALL_OK) {
      return report_status("wait for closure", status);
    }
  }
}

static bool print_rate(char mode, uint64_t bytes, uint64_t started, uint64_t finished,
    const char *scope)
{
  double seconds = (double)(finished - started) / 1000000000.0;
  int written = finished == started ?
      printf("ttcp-%c: %llu bytes; elapsed below clock resolution, rate unavailable\n",
          mode, (unsigned long long)bytes) :
      printf("ttcp-%c: %llu bytes in %.6f seconds = %.3f MiB/s (%s)\n",
          mode, (unsigned long long)bytes, seconds, (double)bytes / 1048576.0 / seconds, scope);
  if (written < 0) {
    perror("ttcp: stdout");
    return false;
  }
  return true;
}

/* Connect to a peer that serves data, such as a host's
 * socat -u OPEN:FILE TCP4-LISTEN:PORT; ordinary sessions cannot listen. */
static int receive_main(const struct options *options, handle_t service, handle_t clock,
    uint32_t address)
{
  bool success = false;
  handle_t stream = HANDLE_INVALID;
  uint64_t deadline, started, finished, received = 0;
  if (printf("ttcp-r: port %u <- %s\n", options->port, options->host) < 0) {
    perror("ttcp: stdout");
    return EXIT_FAILURE;
  }
  if (!next_deadline(clock, &deadline)) {
    return EXIT_FAILURE;
  }
  struct tcp_connect_reply connection;
  enum call_status status = tcp_connect(service, address, options->port, deadline, &connection);
  if (status != CALL_OK) {
    report_status("connect", status);
    return EXIT_FAILURE;
  }
  stream = connection.handle;
  if (read_clock(clock, &started) && receive(stream, clock, &received, &finished) &&
      print_rate('r', received, started, finished, "to peer EOF")) {
    success = true;
  }
  if (success) {
    status = tcp_shutdown_write(stream);
    if (status != CALL_OK) {
      success = report_status("shutdown write", status);
    }
  } else {
    fprintf(stderr, "ttcp: incomplete; %llu bytes received\n", (unsigned long long)received);
    status = tcp_abort(stream);
    if (status != CALL_OK) {
      report_status("abort", status);
    }
  }
  if (handle_close(stream) != 0) {
    fputs("ttcp: cannot close stream\n", stderr);
    success = false;
  }
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("Usage: ttcp -t [-p PORT] [-n BUFFERS] [-l BYTES] HOST\n"
          "       ttcp -r [-p PORT] HOST\n", stderr);
    return EXIT_FAILURE;
  }
  /* Both factors are positive 32-bit values; widen before multiplying. */
  uint64_t total = (uint64_t)options.buffers * options.length;
  handle_t service = startup_resource("tcp"), clock = startup_resource("clock");
  if (service == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("ttcp: missing tcp or clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  uint32_t address;
  if (!udp_parse_address(options.host, &address) &&
      !dns_resolve_address("ttcp", options.host, clock, &address)) {
    return EXIT_FAILURE;
  }
  if (!options.transmit) {
    return receive_main(&options, service, clock, address);
  }
  unsigned char *buffer = malloc(options.length);
  if (!buffer) {
    perror("ttcp: allocate source buffer");
    return EXIT_FAILURE;
  }
  fill_pattern(buffer, options.length);

  bool success = false;
  handle_t stream = HANDLE_INVALID;
  uint64_t deadline, started, finished, accepted = 0;
  if (printf("ttcp-t: %u buffers of %u bytes, port %u -> %s\n",
      options.buffers, options.length, options.port, options.host) < 0) {
    perror("ttcp: stdout");
    goto done;
  }
  if (!next_deadline(clock, &deadline)) {
    goto done;
  }
  struct tcp_connect_reply connection;
  enum call_status status = tcp_connect(service, address, options.port, deadline, &connection);
  if (status != CALL_OK) {
    report_status("connect", status);
    goto done;
  }
  stream = connection.handle;
  if (!read_clock(clock, &started) ||
      !transmit(stream, clock, buffer, &options, &accepted) ||
      !finish_transfer(stream, clock, &finished)) {
    goto done;
  }

  if (!print_rate('t', total, started, finished, "including closure")) {
    goto done;
  }
  success = true;

done:
  if (stream != HANDLE_INVALID) {
    if (!success) {
      fprintf(stderr, "ttcp: incomplete; %llu of %llu bytes accepted locally\n",
          (unsigned long long)accepted, (unsigned long long)total);
      status = tcp_abort(stream);
      if (status != CALL_OK) {
        report_status("abort", status);
      }
    }
    if (handle_close(stream) != 0) {
      fputs("ttcp: cannot close stream\n", stderr);
      success = false;
    }
  }
  free(buffer);
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
