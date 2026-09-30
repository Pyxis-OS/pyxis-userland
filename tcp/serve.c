#include "serve.h"
#include <clock.h>
#include <handle.h>
#include <startup.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tcp.h>
#include <wait.h>

#define CLIENT_LIMIT 4
#define OUTPUT_BYTES 4096
#define SERVICE_OPERATIONS 4
#define SERVICE_BYTES 8192
#define WAIT_NS UINT64_C(10000000000)

struct echo_client {
  handle_t stream;
  unsigned char output[OUTPUT_BYTES];
  size_t start;
  size_t length;
  bool peer_fin;
  bool eof;
};

static bool report_status(const char *operation, enum call_status status)
{
  fprintf(stderr, "tcp: %s failed (status %u)\n", operation, (unsigned)status);
  return false;
}

static bool next_deadline(handle_t clock, uint64_t *deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return report_status("clock", status);
  }
  if (now > UINT64_MAX - WAIT_NS) {
    fputs("tcp: cannot represent deadline\n", stderr);
    return false;
  }
  *deadline = now + WAIT_NS;
  return true;
}

static bool close_client(struct echo_client *client, bool orderly)
{
  bool success = orderly;
  if (!orderly) {
    enum call_status status = tcp_abort(client->stream);
    if (status != CALL_OK) {
      report_status("abort", status);
    }
  }
  if (handle_close(client->stream) != 0) {
    fputs("tcp: cannot close stream\n", stderr);
    success = false;
  }
  client->stream = HANDLE_INVALID;
  return success;
}

static uint64_t client_interests(const struct echo_client *client)
{
  uint64_t events = 0;
  if (!client->eof) {
    if (client->length < sizeof(client->output)) {
      events |= WAIT_READABLE;
    } else if (!client->peer_fin) {
      /* Observe FIN once while input is paused by a full output buffer. */
      events |= WAIT_PEER_FIN;
    }
  }
  if (client->length) {
    events |= WAIT_WRITABLE;
  }
  return events;
}

static bool service_client(struct echo_client *client, uint64_t events)
{
  if (events & WAIT_PEER_FIN) {
    client->peer_fin = true;
  }
  bool readable = events & (WAIT_READABLE | WAIT_PEER_FIN | WAIT_ERROR);
  bool writable = events & (WAIT_WRITABLE | WAIT_WRITE_CLOSED | WAIT_ERROR);
  bool write_blocked = false;
  size_t remaining = SERVICE_BYTES;
  unsigned operations = 0;
  while (operations < SERVICE_OPERATIONS && remaining) {
    bool progress = false;
    if (writable && client->length) {
      size_t length = client->length < remaining ? client->length : remaining;
      struct tcp_write_reply reply;
      ++operations;
      enum call_status status = tcp_try_write(client->stream,
          client->output + client->start, length, &reply);
      if (status == CALL_WOULD_BLOCK) {
        writable = false;
        write_blocked = true;
      } else if (status != CALL_OK) {
        report_status("write", status);
        return close_client(client, false);
      } else {
        client->start += reply.length;
        client->length -= reply.length;
        remaining -= reply.length;
        progress = true;
      }
    }
    if (readable && !client->eof && operations < SERVICE_OPERATIONS && remaining &&
        client->length < sizeof(client->output)) {
      if (client->start) {
        memmove(client->output, client->output + client->start, client->length);
        client->start = 0;
      }
      size_t capacity = sizeof(client->output) - client->length;
      if (capacity > remaining) {
        capacity = remaining;
      }
      struct tcp_read_reply reply;
      ++operations;
      enum call_status status = tcp_try_read(client->stream,
          client->output + client->length, capacity, &reply);
      if (status == CALL_WOULD_BLOCK) {
        readable = false;
      } else if (status != CALL_OK) {
        report_status("read", status);
        return close_client(client, false);
      } else {
        client->length += reply.length;
        remaining -= reply.length;
        client->eof = !reply.length;
        progress = true;
        /* Newly queued output gets one immediate try before waiting for space. */
        if (reply.length) {
          writable = !write_blocked;
        }
      }
    }
    if (!progress) {
      break;
    }
  }
  if (client->eof && !client->length) {
    enum call_status status = tcp_shutdown_write(client->stream);
    if (status != CALL_OK) {
      report_status("shutdown write", status);
      return close_client(client, false);
    }
    return close_client(client, true);
  }
  return true;
}

static bool close_listener(handle_t *listener)
{
  if (handle_close(*listener) != 0) {
    fputs("tcp: cannot close listener\n", stderr);
    return false;
  }
  *listener = HANDLE_INVALID;
  return true;
}

int tcp_serve(unsigned count)
{
  handle_t listener = startup_resource("tcp_listener"), clock = startup_resource("clock");
  if (listener == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("tcp: missing tcp_listener or clock capability\n", stderr);
    return EXIT_FAILURE;
  }
  struct echo_client clients[CLIENT_LIMIT] = {0};
  for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
    clients[i].stream = HANDLE_INVALID;
  }
  bool success = true;
  struct tcp_listener_info info;
  enum call_status status = tcp_listener_inspect(listener, &info);
  if (status != CALL_OK) {
    success = report_status("inspect listener", status);
    goto done;
  }
  if (printf("tcp: listening on %u.%u.%u.%u:%u\n", info.local_address >> 24,
      (info.local_address >> 16) & 255, (info.local_address >> 8) & 255,
      info.local_address & 255, info.local_port) < 0) {
    perror("tcp: stdout");
    success = false;
    goto done;
  }

  unsigned admitted = 0, first = 0;
  for (;;) {
    struct wait_interest interests[CLIENT_LIMIT + 1];
    uint64_t events[CLIENT_LIMIT + 1];
    size_t client_entries[CLIENT_LIMIT], entries = 0;
    unsigned active = 0;
    for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
      client_entries[i] = SIZE_MAX;
      if (clients[i].stream != HANDLE_INVALID) {
        client_entries[i] = entries;
        interests[entries++] = (struct wait_interest){clients[i].stream,
            client_interests(&clients[i])};
        ++active;
      }
    }
    if (listener == HANDLE_INVALID && !active) {
      break;
    }
    size_t listener_entry = SIZE_MAX;
    if (listener != HANDLE_INVALID && active < CLIENT_LIMIT) {
      listener_entry = entries;
      interests[entries++] = (struct wait_interest){listener, WAIT_ACCEPTABLE};
    }
    uint64_t deadline;
    if (!next_deadline(clock, &deadline)) {
      success = false;
      goto done;
    }
    status = wait_many(interests, entries, deadline, events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      success = report_status("wait", status);
      goto done;
    }
    for (unsigned step = 0; step < CLIENT_LIMIT; ++step) {
      unsigned i = (first + step) % CLIENT_LIMIT;
      if (client_entries[i] != SIZE_MAX && events[client_entries[i]]) {
        if (!service_client(&clients[i], events[client_entries[i]])) {
          success = false;
        }
      }
    }
    first = (first + 1) % CLIENT_LIMIT;

    if (listener_entry == SIZE_MAX || !events[listener_entry]) {
      continue;
    }
    /* Admission is bounded by the four free client slots in this pass. */
    for (unsigned i = 0; i < CLIENT_LIMIT && listener != HANDLE_INVALID; ++i) {
      if (clients[i].stream != HANDLE_INVALID) {
        continue;
      }
      struct tcp_accept_reply reply;
      status = tcp_try_accept(listener, &reply);
      if (status == CALL_WOULD_BLOCK) {
        break;
      }
      if (status != CALL_OK) {
        success = report_status("accept", status);
        goto done;
      }
      clients[i] = (struct echo_client){.stream = reply.handle};
      if (count && ++admitted == count && !close_listener(&listener)) {
        success = false;
        goto done;
      }
      if (printf("tcp: accepted %u.%u.%u.%u:%u\n", reply.connection.remote_address >> 24,
          (reply.connection.remote_address >> 16) & 255,
          (reply.connection.remote_address >> 8) & 255,
          reply.connection.remote_address & 255, reply.connection.remote_port) < 0) {
        perror("tcp: stdout");
        success = false;
        goto done;
      }
    }
  }

done:
  if (listener != HANDLE_INVALID && !close_listener(&listener)) {
    success = false;
  }
  for (unsigned i = 0; i < CLIENT_LIMIT; ++i) {
    if (clients[i].stream != HANDLE_INVALID) {
      close_client(&clients[i], false);
      success = false;
    }
  }
  return success && !ferror(stdout) && !ferror(stderr) ? EXIT_SUCCESS : EXIT_FAILURE;
}
