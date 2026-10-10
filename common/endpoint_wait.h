#ifndef USERSPACE_DEMO_ENDPOINT_WAIT_H
#define USERSPACE_DEMO_ENDPOINT_WAIT_H

#include <clock.h>
#include <console.h>
#include <endpoint.h>
#include <wait.h>

struct demo_endpoint_wait {
  bool enabled;
  handle_t input;
  handle_t output;
  handle_t clock;
};

static enum call_status demo_wait_sources(const struct demo_endpoint_wait *wait,
    handle_t receiver, bool *readable, bool *input_read)
{
  struct wait_interest interests[] = {
    {.handle = receiver, .events = WAIT_READABLE},
    {.handle = wait->input, .events = WAIT_READABLE},
  };
  *readable = false;
  *input_read = false;
  uint64_t now;
  enum call_status status = clock_now(wait->clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
    return CALL_BAD_REQUEST;
  }
  uint64_t events[2];
  status = wait_many(interests, 2, now + WAIT_MAX_WAIT_NS, events);
  if (status != CALL_OK) {
    return status;
  }
  if (events[0] & WAIT_CLOSED) {
    return CALL_ENDPOINT_CLOSED;
  }
  if (events[1] & (WAIT_READABLE | WAIT_PEER_FIN | WAIT_ERROR)) {
    char bytes[32];
    size_t count;
    status = console_read_timeout(wait->input, bytes, sizeof(bytes), 0, &count);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      return status;
    }
    if (status == CALL_OK) {
      if (count == 0) {
        return CALL_ENDPOINT_CLOSED;
      }
      *input_read = true;
      status = console_print(wait->output, "wait: console input\n");
      if (status != CALL_OK) {
        return status;
      }
    }
  }
  *readable = (events[0] & WAIT_READABLE) != 0;
  return CALL_OK;
}

static bool demo_wait_start(const struct demo_endpoint_wait *wait, handle_t receiver)
{
  if (!wait->enabled) {
    return true;
  }
  if (console_print(wait->output,
      "wait: receiver and console; type a key to start clients\n") != CALL_OK) {
    return false;
  }
  for (;;) {
    bool readable, input_read;
    enum call_status status = demo_wait_sources(wait, receiver, &readable, &input_read);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      return false;
    }
    if (input_read) {
      return true;
    }
  }
}

static enum call_status demo_endpoint_receive(const struct demo_endpoint_wait *wait,
    handle_t receiver, struct endpoint_packet *packet)
{
  if (wait->enabled) {
    for (;;) {
      bool readable, input_read;
      enum call_status status = demo_wait_sources(wait, receiver, &readable, &input_read);
      if (status != CALL_OK && status != CALL_TIMED_OUT) {
        return status;
      }
      if (readable) {
        break;
      }
    }
  }
  /* Observation reserves nothing; a queued CALL may expire before RECEIVE. */
  enum call_status status = endpoint_receive(receiver, packet);
  if (status == CALL_OK && wait->enabled) {
    const char *text;
    switch (packet->kind) {
      case ENDPOINT_MESSAGE_CALL: text = "wait: received CALL\n"; break;
      case ENDPOINT_MESSAGE_SEND: text = "wait: received SEND\n"; break;
      case ENDPOINT_MESSAGE_CANCEL: text = "wait: received CANCEL\n"; break;
      case ENDPOINT_MESSAGE_RETIRE: text = "wait: received RETIRE\n"; break;
      default: return CALL_BAD_REQUEST;
    }
    /* The delivery already belongs to the caller even if logging fails. */
    console_print(wait->output, text);
  }
  return status;
}

#endif
