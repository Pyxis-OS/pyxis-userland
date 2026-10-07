#include "session.h"
#include <clock.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <remote/terminal.h>
#include <remote/beacon.h>
#include <startup.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>
#include <wait.h>
#include <udp.h>

#define CLIENT_LIMIT 4
#define SERVICE_ROUNDS 4
#define HELLO_NS UINT64_C(10000000000)
#define DRAIN_NS UINT64_C(5000000000)
#define IDLE_WAIT_NS UINT64_C(10000000000)
#define RECONNECT_NS UINT64_C(1000000000)
#define FRAME_SIZE (REMOTE_HEADER_SIZE + REMOTE_PAYLOAD_MAX)

/* Native completion kinds cross the wire by value. */
static_assert(REMOTE_COMPLETION_EXITED == TERMINAL_COMPLETION_EXITED);
static_assert(REMOTE_COMPLETION_FAULTED == TERMINAL_COMPLETION_FAULTED);
static_assert(REMOTE_COMPLETION_TERMINATED == TERMINAL_COMPLETION_TERMINATED);
static_assert(REMOTE_COMPLETION_LAUNCH_FAILED == TERMINAL_COMPLETION_LAUNCH_FAILED);
static_assert(REMOTE_COMPLETION_BUILTIN == TERMINAL_COMPLETION_BUILTIN);
static_assert(REMOTE_COMPLETION_REJECTED == TERMINAL_COMPLETION_REJECTED);
static_assert(REMOTE_COMPLETION_LAUNCHED == TERMINAL_COMPLETION_LAUNCHED);

struct client {
  bool occupied;
  handle_t stream;
  struct remote_shell shell;
  bool ready;
  bool closing;
  bool group_complete;
  bool shell_ready;
  bool group_ready;
  bool input_ended;
  bool output_eof;
  bool final_sent;
  bool shutdown;
  uint32_t cause;
  uint32_t error;
  struct process_result result;
  uint64_t deadline;
  unsigned char input[FRAME_SIZE];
  size_t received;
  size_t injected;
  unsigned char output[FRAME_SIZE];
  size_t output_start;
  size_t output_length;
  uint32_t output_type;
};

static bool failed;

static void report(const char *operation, enum call_status status)
{
  fprintf(stderr, "remote-terminal: %s failed (status %u)\n", operation, (unsigned)status);
}

static void close_handle(handle_t *handle)
{
  if (*handle != HANDLE_INVALID) {
    if (handle_close(*handle) != 0) {
      fputs("remote-terminal: cannot close owned handle\n", stderr);
      failed = true;
    }
    *handle = HANDLE_INVALID;
  }
}

static void queue_frame(struct client *client, uint32_t type, const void *data, size_t length)
{
  remote_encode_u32(client->output, type);
  remote_encode_u32(client->output + 4, (uint32_t)length);
  if (length) {
    memcpy(client->output + REMOTE_HEADER_SIZE, data, length);
  }
  client->output_start = 0;
  client->output_length = REMOTE_HEADER_SIZE + length;
  client->output_type = type;
}

static void begin_close(struct client *client, uint32_t cause, uint64_t now)
{
  if (client->closing) {
    return;
  }
  client->closing = true;
  client->cause = cause;
  client->deadline = now + DRAIN_NS;
  client->received = 0;
  client->injected = 0;
  if (client->shell.group != HANDLE_INVALID) {
    enum call_status status = execution_group_terminate(client->shell.group);
    if (status != CALL_OK) {
      report("terminate", status);
      failed = true;
    }
  } else {
    client->group_complete = true;
  }
}

static void disconnect(struct client *client, uint64_t now, bool orderly)
{
  begin_close(client, REMOTE_CAUSE_CLIENT_CLOSE, now);
  if (client->stream != HANDLE_INVALID) {
    if (!orderly) {
      tcp_abort(client->stream);
    }
    close_handle(&client->stream);
  }
  /* Disconnect abandons presentation, while supervision stays alive until
   * group completion, including outstanding kernel ownership handoffs. */
  close_handle(&client->shell.attachment);
  client->output_length = 0;
  client->output_eof = true;
}

static void reject(struct client *client, uint32_t error, uint64_t now)
{
  client->error = error;
  begin_close(client, REMOTE_CAUSE_SERVER_ERROR, now);
}

static void observe_completion(struct client *client, uint64_t now)
{
  if (client->shell_ready || client->group_ready) {
    if (client->shell.process != HANDLE_INVALID) {
      enum call_status status = process_wait(client->shell.process, &client->result);
      if (status != CALL_OK) {
        report("shell completion", status);
        failed = true;
        return;
      }
      close_handle(&client->shell.process);
    }
    if (!client->closing) {
      begin_close(client, REMOTE_CAUSE_SHELL_EXIT, now);
    }
    client->shell_ready = false;
  }
  if (client->group_ready) {
    enum call_status status = execution_group_wait(client->shell.group);
    if (status != CALL_OK) {
      report("group completion", status);
      failed = true;
      return;
    }
    close_handle(&client->shell.group);
    client->group_complete = true;
    client->group_ready = false;
  }
}

static bool valid_header(const struct client *client)
{
  uint32_t type = remote_decode_u32(client->input);
  uint32_t length = remote_decode_u32(client->input + 4);
  if (!client->ready) {
    return type == REMOTE_HELLO && length == REMOTE_HELLO_SIZE;
  }
  return (type == REMOTE_INPUT && !client->input_ended && length && length <= REMOTE_PAYLOAD_MAX) ||
      ((type == REMOTE_END_INPUT || type == REMOTE_CLOSE) && !length);
}

static bool consume_frame(struct client *client, unsigned tab_width, uint64_t now)
{
  uint32_t type = remote_decode_u32(client->input);
  size_t length = remote_decode_u32(client->input + 4);
  const unsigned char *payload = client->input + REMOTE_HEADER_SIZE;
  enum call_status status;
  switch (type) {
  case REMOTE_HELLO: {
    uint32_t columns = remote_decode_u32(payload), rows = remote_decode_u32(payload + 4);
    uint32_t options = remote_decode_u32(payload + 8);
    if (!columns || columns > REMOTE_COLUMNS_MAX || !rows || rows > REMOTE_ROWS_MAX ||
        (options & ~REMOTE_OPTIONS)) {
      reject(client, REMOTE_ERROR_BAD_FRAME, now);
      return true;
    }
    status = remote_shell_launch(columns, rows, tab_width,
        options & REMOTE_OPTION_NO_SHELL_ECHO, &client->shell);
    if (status != CALL_OK) {
      report("launch shell", status);
      reject(client, REMOTE_ERROR_LAUNCH, now);
      return true;
    }
    client->ready = true;
    client->deadline = 0;
    queue_frame(client, REMOTE_READY, NULL, 0);
    break;
  }
  case REMOTE_INPUT: {
    struct terminal_transfer_reply reply;
    status = terminal_try_inject(client->shell.attachment, payload + client->injected,
        length - client->injected, &reply);
    if (status == CALL_WOULD_BLOCK) {
      return false;
    }
    if (status == CALL_ENDPOINT_CLOSED) {
      /* The last reader can close before its process completion is published.
       * Discard this accepted frame and wait for lifecycle observation. */
      client->input_ended = true;
    } else if (status != CALL_OK) {
      reject(client, REMOTE_ERROR_INTERNAL, now);
      return true;
    } else {
      client->injected += reply.length;
      if (client->injected != length) {
        return true;
      }
    }
    break;
  }
  case REMOTE_END_INPUT:
    status = terminal_end_input(client->shell.attachment);
    if (status != CALL_OK) {
      reject(client, REMOTE_ERROR_INTERNAL, now);
      return true;
    }
    client->input_ended = true;
    break;
  case REMOTE_CLOSE:
    begin_close(client, REMOTE_CAUSE_CLIENT_CLOSE, now);
    return true;
  default:
    reject(client, REMOTE_ERROR_BAD_FRAME, now);
    return true;
  }
  client->received = 0;
  client->injected = 0;
  return true;
}

static bool read_input(struct client *client, unsigned tab_width, uint64_t now)
{
  if (client->closing) {
    /* Discard late input while closing, allowing orderly TCP teardown after
     * FINAL. This bounded read also observes disconnect during cleanup. */
    unsigned char discard[REMOTE_PAYLOAD_MAX];
    struct tcp_read_reply reply;
    enum call_status status = tcp_try_read(client->stream, discard, sizeof(discard), &reply);
    if (status == CALL_WOULD_BLOCK) {
      return false;
    }
    if (status != CALL_OK || !reply.length) {
      disconnect(client, now, status == CALL_OK && client->shutdown);
    }
    return true;
  }
  size_t target = REMOTE_HEADER_SIZE;
  if (client->received >= REMOTE_HEADER_SIZE) {
    if (!valid_header(client)) {
      reject(client, REMOTE_ERROR_BAD_FRAME, now);
      return true;
    }
    target += remote_decode_u32(client->input + 4);
  }
  if (client->received == target && target >= REMOTE_HEADER_SIZE) {
    return consume_frame(client, tab_width, now);
  }
  struct tcp_read_reply reply;
  enum call_status status = tcp_try_read(client->stream, client->input + client->received,
      target - client->received, &reply);
  if (status == CALL_WOULD_BLOCK) {
    return false;
  }
  if (status != CALL_OK || !reply.length) {
    disconnect(client, now, false);
    return true;
  }
  client->received += reply.length;
  return true;
}

static bool write_output(struct client *client, uint64_t now)
{
  if (!client->output_length) {
    return false;
  }
  struct tcp_write_reply reply;
  enum call_status status = tcp_try_write(client->stream,
      client->output + client->output_start, client->output_length, &reply);
  if (status == CALL_WOULD_BLOCK) {
    return false;
  }
  if (status != CALL_OK) {
    disconnect(client, now, false);
    return true;
  }
  client->output_start += reply.length;
  client->output_length -= reply.length;
  if (!client->output_length && client->output_type == REMOTE_FINAL) {
    client->final_sent = true;
  }
  return true;
}

static bool collect_output(struct client *client, uint64_t now)
{
  if (client->output_length || client->final_sent || client->shutdown) {
    return false;
  }
  if (client->error) {
    unsigned char payload[REMOTE_ERROR_SIZE];
    remote_encode_u32(payload, client->error);
    queue_frame(client, REMOTE_ERROR, payload, sizeof(payload));
    client->error = 0;
    return true;
  }
  if (!client->ready || client->output_eof) {
    return false;
  }
  unsigned char record[TERMINAL_RECORD_MAX];
  struct terminal_transfer_reply reply;
  enum call_status status = terminal_try_drain(client->shell.attachment, record, sizeof(record), &reply);
  if (status == CALL_WOULD_BLOCK) {
    return false;
  }
  if (status != CALL_OK) {
    report("drain terminal", status);
    disconnect(client, now, false);
    return true;
  }
  if (!reply.length) {
    client->output_eof = true;
    return true;
  }
  struct terminal_record header;
  memcpy(&header, record, sizeof(header));
  const unsigned char *payload = record + sizeof(header);
  if (header.type == TERMINAL_RECORD_DATA) {
    queue_frame(client, REMOTE_OUTPUT, payload, header.length);
  } else if (header.type == TERMINAL_RECORD_FRESH_LINE) {
    queue_frame(client, REMOTE_FRESH_LINE, NULL, 0);
  } else if (header.type == TERMINAL_RECORD_COMMAND_COMPLETE) {
    struct terminal_command_complete completion;
    unsigned char encoded[REMOTE_COMMAND_COMPLETE_SIZE];
    memcpy(&completion, payload, sizeof(completion));
    remote_encode_u64(encoded, completion.command);
    remote_encode_u32(encoded + 8, (uint32_t)completion.kind);
    remote_encode_u32(encoded + 12, (uint32_t)completion.status);
    queue_frame(client, REMOTE_COMMAND_COMPLETE, encoded, sizeof(encoded));
  } else {
    uint64_t width;
    unsigned char encoded[REMOTE_TAB_WIDTH_SIZE];
    memcpy(&width, payload, sizeof(width));
    remote_encode_u64(encoded, width);
    queue_frame(client, REMOTE_TAB_WIDTH, encoded, sizeof(encoded));
  }
  return true;
}

static bool finish_output(struct client *client, uint64_t now)
{
  if (!client->closing || !client->group_complete || client->output_length || client->error) {
    return false;
  }
  if (client->ready && client->output_eof && !client->final_sent) {
    unsigned char payload[REMOTE_FINAL_SIZE];
    remote_encode_u32(payload, client->cause);
    remote_encode_u32(payload + 4, (uint32_t)client->result.kind);
    remote_encode_u32(payload + 8, (uint32_t)client->result.exit_status);
    remote_encode_u32(payload + 12, REMOTE_DRAIN_COMPLETE);
    queue_frame(client, REMOTE_FINAL, payload, sizeof(payload));
    return true;
  }
  if ((!client->ready || client->final_sent) && !client->shutdown) {
    enum call_status status = tcp_shutdown_write(client->stream);
    if (status != CALL_OK) {
      disconnect(client, now, false);
    } else {
      client->shutdown = true;
    }
    return true;
  }
  return false;
}

static bool service_client(struct client *client, unsigned tab_width, uint64_t now)
{
  observe_completion(client, now);
  if (client->deadline && now >= client->deadline && client->stream != HANDLE_INVALID) {
    if (!client->closing) {
      reject(client, REMOTE_ERROR_HELLO_TIMEOUT, now);
    } else {
      fprintf(stderr, "remote-terminal: final output/close deadline expired\n");
      disconnect(client, now, false);
    }
  }
  bool progress = false;
  for (unsigned round = 0; round < SERVICE_ROUNDS && client->stream != HANDLE_INVALID; ++round) {
    progress = write_output(client, now);
    if (client->stream == HANDLE_INVALID) {
      break;
    }
    progress |= read_input(client, tab_width, now);
    if (client->stream == HANDLE_INVALID) {
      break;
    }
    progress |= collect_output(client, now);
    if (client->stream == HANDLE_INVALID) {
      break;
    }
    progress |= finish_output(client, now);
    if (!progress) {
      break;
    }
  }
  if (client->stream == HANDLE_INVALID && client->group_complete) {
    close_handle(&client->shell.attachment);
    close_handle(&client->shell.process);
    *client = (struct client){0};
    return true;
  }
  return progress;
}

static uint64_t stream_interests(const struct client *client)
{
  uint64_t interests = WAIT_PEER_FIN;
  bool input_pending = client->received >= REMOTE_HEADER_SIZE &&
      client->received == REMOTE_HEADER_SIZE + remote_decode_u32(client->input + 4);
  if (client->closing || !input_pending) {
    interests |= WAIT_READABLE;
  }
  if (client->output_length) {
    interests |= WAIT_WRITABLE;
  }
  return interests;
}

static uint64_t terminal_interests(const struct client *client)
{
  uint64_t interests = 0;
  if (client->ready && !client->output_eof && !client->output_length) {
    interests |= WAIT_READABLE;
  }
  if (!client->closing && client->received >= REMOTE_HEADER_SIZE &&
      remote_decode_u32(client->input) == REMOTE_INPUT &&
      client->received == REMOTE_HEADER_SIZE + remote_decode_u32(client->input + 4)) {
    interests |= WAIT_WRITABLE;
  }
  return interests;
}

/* The endpoint exists only while discovering. Shutdown before connecting drops
 * every queued beacon, so a later session requires a fresh advertisement. */
static handle_t discover_remote(handle_t service, handle_t tcp, handle_t clock,
    const char *name)
{
  struct udp_open_reply opened;
  enum call_status status = udp_open_broadcast(service, REMOTE_BEACON_PORT, &opened);
  if (status != CALL_OK) {
    report("open beacon endpoint", status);
    failed = true;
    return HANDLE_INVALID;
  }
  handle_t endpoint = opened.handle;
  uint32_t address = 0;
  uint16_t port = 0;
  while (!failed) {
    uint64_t now;
    status = clock_now(clock, &now);
    if (status != CALL_OK || now > UINT64_MAX - IDLE_WAIT_NS) {
      report("beacon clock", status);
      failed = true;
      break;
    }
    unsigned char packet[UDP_MAX_PAYLOAD];
    struct udp_receive_reply received;
    status = udp_receive(endpoint, packet, sizeof(packet), now + IDLE_WAIT_NS, &received);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      report("receive beacon", status);
      failed = true;
      break;
    }
    if (remote_beacon_decode(packet, received.length, name, &port) && received.address &&
        received.address != UINT32_MAX && (received.address >> 28) != 14) {
      address = received.address;
      break;
    }
  }
  status = udp_shutdown(endpoint);
  if (status != CALL_OK) {
    report("shutdown beacon endpoint", status);
    failed = true;
  }
  close_handle(&endpoint);
  if (failed) {
    return HANDLE_INVALID;
  }
  uint64_t now;
  status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - HELLO_NS) {
    report("connect clock", status);
    failed = true;
    return HANDLE_INVALID;
  }
  struct tcp_connect_reply connected;
  status = tcp_connect(tcp, address, port, now + HELLO_NS, &connected);
  if (status == CALL_OK) {
    printf("remote-terminal: connected to %u.%u.%u.%u:%u\n", address >> 24,
        (address >> 16) & 255, (address >> 8) & 255, address & 255, port);
    return connected.handle;
  }
  report("reverse connect", status);
  status = clock_sleep_for(clock, RECONNECT_NS);
  if (status != CALL_OK) {
    report("reconnect delay", status);
    failed = true;
  }
  return HANDLE_INVALID;
}

int main(int argc, char **argv)
{
  unsigned tab_width = 8;
  const char *beacon = NULL;
  if (argc == 4 && !strcmp(argv[2], "--beacon") && remote_beacon_name_length(argv[3])) {
    beacon = argv[3];
  } else if (argc != 1 && argc != 2) {
    return EXIT_FAILURE;
  }
  if (argc >= 2) {
    char *end;
    unsigned long width = strtoul(argv[1], &end, 10);
    if (!*argv[1] || *end || width < 1 || width > 32) {
      return EXIT_FAILURE;
    }
    tab_width = (unsigned)width;
  }
  handle_t listener = startup_resource("tcp_listener"), clock = startup_resource("clock");
  handle_t udp_beacons = startup_resource("udp_beacons"), tcp = startup_resource("tcp");
  if ((beacon ? udp_beacons == HANDLE_INVALID || tcp == HANDLE_INVALID :
      listener == HANDLE_INVALID) || clock == HANDLE_INVALID ||
      startup_resource("terminal") == HANDLE_INVALID || startup_resource("launcher") == HANDLE_INVALID ||
      startup_resource("memory") == HANDLE_INVALID || startup_root("boot") == HANDLE_INVALID ||
      startup_root("tmp") == HANDLE_INVALID) {
    fputs("remote-terminal: missing startup authority\n", stderr);
    return EXIT_FAILURE;
  }
  /* Heap storage keeps four bounded duplex frames off the userspace stack. */
  struct client *clients = calloc(CLIENT_LIMIT, sizeof(*clients));
  if (!clients) {
    return EXIT_FAILURE;
  }
  unsigned limit = beacon ? 1 : CLIENT_LIMIT;
  unsigned first = 0;
  bool accept_ready = true;
  while (!failed) {
    if (beacon && !clients[0].occupied) {
      handle_t stream = discover_remote(udp_beacons, tcp, clock, beacon);
      if (stream == HANDLE_INVALID) {
        continue;
      }
      uint64_t connected_at;
      enum call_status status = clock_now(clock, &connected_at);
      if (status != CALL_OK || connected_at > UINT64_MAX - HELLO_NS) {
        report("session clock", status);
        tcp_abort(stream);
        close_handle(&stream);
        break;
      }
      clients[0] = (struct client){.occupied = true, .stream = stream,
          .deadline = connected_at + HELLO_NS};
    }
    uint64_t now;
    enum call_status status = clock_now(clock, &now);
    if (status != CALL_OK || now > UINT64_MAX - IDLE_WAIT_NS) {
      report("clock", status);
      break;
    }
    bool progress = false;
    for (unsigned step = 0; step < limit; ++step) {
      unsigned index = (first + step) % limit;
      if (clients[index].occupied) {
        progress |= service_client(&clients[index], tab_width, now);
      }
    }
    first = (first + 1) % limit;
    if (!beacon && accept_ready) {
      for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
        if (clients[i].occupied) {
          continue;
        }
        struct tcp_accept_reply reply;
        status = tcp_try_accept(listener, &reply);
        if (status == CALL_WOULD_BLOCK) {
          break;
        }
        if (status != CALL_OK) {
          report("accept", status);
          failed = true;
          break;
        }
        clients[i] = (struct client){.occupied = true, .stream = reply.handle, .deadline = now + HELLO_NS};
        progress = true;
      }
    }
    struct wait_interest interests[CLIENT_LIMIT * 3 + 1];
    uint64_t events[CLIENT_LIMIT * 3 + 1];
    size_t stream_entries[CLIENT_LIMIT], lifecycle_entries[CLIENT_LIMIT];
    size_t count = 0;
    unsigned active = 0;
    uint64_t deadline = now + IDLE_WAIT_NS;
    for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
      struct client *client = &clients[i];
      stream_entries[i] = lifecycle_entries[i] = SIZE_MAX;
      if (!client->occupied) {
        continue;
      }
      ++active;
      if (client->stream != HANDLE_INVALID) {
        stream_entries[i] = count;
        interests[count++] = (struct wait_interest){client->stream, stream_interests(client)};
        if (client->deadline && client->deadline < deadline) {
          deadline = client->deadline;
        }
      }
      uint64_t terminal_events = terminal_interests(client);
      if (client->shell.attachment != HANDLE_INVALID && terminal_events) {
        interests[count++] = (struct wait_interest){client->shell.attachment, terminal_events};
      }
      handle_t lifecycle = client->closing ? client->shell.group : client->shell.process;
      if (lifecycle != HANDLE_INVALID) {
        lifecycle_entries[i] = count;
        interests[count++] = (struct wait_interest){lifecycle, WAIT_COMPLETE};
      }
    }
    size_t listener_entry = SIZE_MAX;
    if (!beacon && active < CLIENT_LIMIT) {
      listener_entry = count;
      interests[count++] = (struct wait_interest){listener, WAIT_ACCEPTABLE};
    }
    if (failed) {
      break;
    }
    if (beacon && !active) {
      continue;
    }
    status = wait_many(interests, count, progress ? 0 : deadline, events);
    if (status == CALL_TIMED_OUT) {
      accept_ready = false;
      continue;
    }
    if (status != CALL_OK) {
      report("wait", status);
      break;
    }
    accept_ready = listener_entry != SIZE_MAX && events[listener_entry];
    for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
      struct client *client = &clients[i];
      if (lifecycle_entries[i] != SIZE_MAX && events[lifecycle_entries[i]]) {
        if (client->closing) {
          client->group_ready = true;
        } else {
          client->shell_ready = true;
        }
      }
      if (stream_entries[i] != SIZE_MAX && events[stream_entries[i]] & WAIT_ERROR) {
        disconnect(client, now, false);
      } else if (stream_entries[i] != SIZE_MAX && events[stream_entries[i]] & WAIT_PEER_FIN) {
        /* The protocol uses END_INPUT for graceful stdin EOF. A TCP half-close
         * is session disconnection, even if unread transport bytes remain. */
        disconnect(client, now, client->shutdown);
      }
    }
  }
  close_handle(&listener);
  for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
    if (clients[i].stream != HANDLE_INVALID) {
      tcp_abort(clients[i].stream);
    }
    close_handle(&clients[i].stream);
    close_handle(&clients[i].shell.attachment);
    close_handle(&clients[i].shell.process);
    close_handle(&clients[i].shell.group);
  }
  free(clients);
  return EXIT_FAILURE;
}
