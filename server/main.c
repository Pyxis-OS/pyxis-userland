#include <abi/console.h>
#include <abi/clock.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <console.h>
#include <clock.h>
#include <directory.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/content_service.h"

static bool close_grants(const struct endpoint_packet *packet)
{
  bool closed = true;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (handle_close(packet->grants[i].handle) != 0) {
      closed = false;
    }
  }
  return closed;
}

static enum call_status open_content(handle_t app, handle_t *content)
{
  handle_t share;
  enum call_status status = directory_lookup(app, "share", DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &share);
  if (status != CALL_OK) {
    return status;
  }
  status = directory_lookup(share, "hello.txt", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, content);
  if (handle_close(share) != 0 && status == CALL_OK) {
    handle_close(*content);
    *content = HANDLE_INVALID;
    return CALL_BAD_HANDLE;
  }
  return status;
}

static enum call_status launch_client(handle_t launcher, handle_t image,
    handle_t output, handle_t memory, handle_t caller, handle_t content, handle_t clock,
    const char *mode, const char *id, bool send_only, handle_t *child)
{
  enum { OUTPUT, MEMORY, CALLER, CONTENT, CLOCK, GRANT_COUNT };
  bool needs_clock = !strcmp(mode, "expired") || !strcmp(mode, "queued-timeout") ||
      !strcmp(mode, "delivered-timeout") || !strcmp(mode, "cancel-full") ||
      !strcmp(mode, "cancel-finish") || !strcmp(mode, "deadline-reply");
  struct launch_grant grants[GRANT_COUNT] = {
    [OUTPUT] = {output, CONSOLE_RIGHT_WRITE, 0},
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [CALLER] = {caller, 0, send_only ? HANDLE_TRANSPORT_SEND : HANDLE_TRANSPORT_CALL},
    [CONTENT] = {content, FILE_RIGHT_READ, 0},
    [CLOCK] = {clock, CLOCK_RIGHT_READ, 0},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"memory", MEMORY},
    {(uintptr_t)"endpoint", CALLER},
    {(uintptr_t)"content", CONTENT},
    {(uintptr_t)"clock", CLOCK},
  };
  const char *arguments[] = {"client.pxe", mode, id};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = needs_clock ? GRANT_COUNT : GRANT_COUNT - 1,
    .resources = (uintptr_t)resources,
    .resource_count = needs_clock ? GRANT_COUNT : GRANT_COUNT - 1,
    .argv = (uintptr_t)arguments,
    .argc = sizeof(arguments) / sizeof(arguments[0]),
  };
  return launcher_launch(launcher, &request, child);
}

static uint64_t print_content(const struct endpoint_packet *packet, handle_t output)
{
  if (packet->grant_count == 0) {
    return CONTENT_INVALID;
  }
  char buffer[128];
  uint64_t offset = 0;
  for (;;) {
    size_t count;
    if (file_read(packet->grants[0].handle, offset, buffer, sizeof(buffer), &count) != CALL_OK) {
      return CONTENT_IO_ERROR;
    }
    if (count == 0) {
      return CONTENT_OK;
    }
    if (count > UINT64_MAX - offset ||
        console_write_all(output, buffer, count) != CALL_OK) {
      return CONTENT_IO_ERROR;
    }
    offset += count;
  }
}

static bool read_grants(const struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    char byte;
    size_t count;
    if (file_read(packet->grants[i].handle, 0, &byte, 1, &count) != CALL_OK || count != 1) {
      return false;
    }
  }
  return true;
}

static bool finish_request(struct endpoint_packet *packet, handle_t output, bool abandon)
{
  bool valid = packet->kind == ENDPOINT_MESSAGE_CALL &&
      (packet->size == sizeof(struct content_request) ||
      packet->size == ENDPOINT_DATA_MAX);
  struct content_request request;
  if (valid) {
    memcpy(&request, packet->data, sizeof(request));
    valid = request.operation == CONTENT_PRINT && request.client >= 1 &&
        request.client <= ENDPOINT_DELIVERIES_MAX && packet->grant_count ==
        (packet->size == ENDPOINT_DATA_MAX ? ENDPOINT_GRANTS_MAX : 1);
  }
  if (valid && packet->size == ENDPOINT_DATA_MAX) {
    for (size_t i = sizeof(request); i < packet->size; ++i) {
      if (packet->data[i] != (uint8_t)i) {
        valid = false;
        break;
      }
    }
  }
  uint64_t application_result = valid ? print_content(packet, output) : CONTENT_INVALID;
  enum call_status status;
  if (abandon) {
    status = handle_close(packet->receipt) == 0 ? CALL_OK : CALL_BAD_HANDLE;
  } else {
    status = endpoint_reply(packet->receipt, application_result,
        packet->data, packet->size, packet->grants, packet->grant_count);
    if (status != CALL_OK) {
      handle_close(packet->receipt);
    }
  }
  return close_grants(packet) && status == CALL_OK;
}

static bool finish_send(struct endpoint_packet *packet, handle_t output, uint64_t client)
{
  bool valid = packet->kind == ENDPOINT_MESSAGE_SEND &&
      packet->size == ENDPOINT_DATA_MAX && packet->grant_count == ENDPOINT_GRANTS_MAX;
  struct content_request request;
  if (valid) {
    memcpy(&request, packet->data, sizeof(request));
    valid = request.operation == CONTENT_PRINT && request.client == client;
  }
  for (size_t i = sizeof(request); valid && i < packet->size; ++i) {
    valid = packet->data[i] == (uint8_t)i;
  }
  enum call_status status = endpoint_finish(packet->receipt);
  /* The received file grants remain owned after finishing the delivery. */
  bool usable = valid && read_grants(packet) && print_content(packet, output) == CONTENT_OK;
  bool closed = close_grants(packet);
  return status == CALL_OK && usable && closed;
}

static bool wait_client(handle_t child)
{
  struct process_result result;
  return process_wait(child, &result) == CALL_OK &&
      result.kind != PROCESS_FAULTED && result.exit_status == 0;
}

static bool from_client_one(const struct endpoint_packet *packet)
{
  if (packet->size < sizeof(struct content_request)) {
    return false;
  }
  struct content_request request;
  memcpy(&request, packet->data, sizeof(request));
  return request.client == 1;
}

static bool receive_sentinel(handle_t caller, handle_t receiver)
{
  if (endpoint_send(caller, NULL, 0, NULL, 0) != CALL_OK) {
    return false;
  }
  struct endpoint_packet packet;
  if (endpoint_receive(receiver, &packet) != CALL_OK) {
    return false;
  }
  bool valid = packet.kind == ENDPOINT_MESSAGE_SEND && packet.size == 0 &&
      packet.grant_count == 0 && packet.deadline_ns == 0;
  return endpoint_finish(packet.receipt) == CALL_OK && valid;
}

static bool receive_cancel(handle_t receiver, const struct endpoint_packet *call)
{
  struct endpoint_packet notice;
  if (endpoint_receive(receiver, &notice) != CALL_OK) {
    return false;
  }
  return notice.kind == ENDPOINT_MESSAGE_CANCEL &&
      notice.receipt == call->receipt && notice.deadline_ns == call->deadline_ns &&
      notice.reason == CALL_TIMED_OUT && notice.object_id == 0 &&
      notice.protocol == 0 && notice.operation == 0 && notice.rights == 0 &&
      notice.size == 0 && notice.grant_count == 0 && notice.result == 0;
}

int main(int argc, char **argv)
{
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--wide") &&
      strcmp(argv[1], "--abandon") && strcmp(argv[1], "--saturate") &&
      strcmp(argv[1], "--close") && strcmp(argv[1], "--exit") &&
      strcmp(argv[1], "--send") && strcmp(argv[1], "--mixed") &&
      strcmp(argv[1], "--expired") && strcmp(argv[1], "--queued-timeout") &&
      strcmp(argv[1], "--delivered-timeout") && strcmp(argv[1], "--cancel-full") &&
      strcmp(argv[1], "--cancel-finish") && strcmp(argv[1], "--deadline-reply"))) {
    return 1;
  }
  const char *mode = argc == 1 ? "normal" : argv[1] + 2;
  bool deadline_mode = !strcmp(mode, "expired") || !strcmp(mode, "queued-timeout") ||
      !strcmp(mode, "delivered-timeout") || !strcmp(mode, "cancel-full") ||
      !strcmp(mode, "cancel-finish") || !strcmp(mode, "deadline-reply");
  handle_t service = startup_resource("service");
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t output = startup_resource("output");
  handle_t clock = startup_resource("clock");
  handle_t app = startup_root("app");
  if (service == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      memory == HANDLE_INVALID || output == HANDLE_INVALID ||
      (deadline_mode && clock == HANDLE_INVALID) ||
      app == HANDLE_INVALID) {
    return 1;
  }

  struct endpoint_create_reply endpoint;
  enum call_status status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    return 1;
  }
  handle_t image = HANDLE_INVALID, content = HANDLE_INVALID;
  handle_t children[ENDPOINT_DELIVERIES_MAX + 1] = {0};
  size_t count = !strcmp(mode, "send") || !strcmp(mode, "mixed") || deadline_mode ? 0 :
      !strcmp(mode, "saturate") ? ENDPOINT_DELIVERIES_MAX :
      (!strcmp(mode, "close") || !strcmp(mode, "exit") ? 1 : 2);
  struct endpoint_packet *packets = malloc(ENDPOINT_DELIVERIES_MAX * sizeof(*packets));
  bool ok = false;
  if (!packets) {
    goto done;
  }
  status = directory_lookup(app, "client.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK || open_content(app, &content) != CALL_OK) {
    goto done;
  }
  for (size_t i = 0; i < count; ++i) {
    char label[3];
    int length = snprintf(label, sizeof(label), "%zu", i + 1);
    if (length < 0 || (size_t)length >= sizeof(label)) {
      goto done;
    }
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, clock, mode, label, false, &children[i]);
    if (status != CALL_OK) {
      goto done;
    }
    if (!strcmp(mode, "saturate") &&
        endpoint_receive(endpoint.receiver, &packets[i]) != CALL_OK) {
      goto done;
    }
  }

  if (deadline_mode) {
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, clock, mode, "1", false, &children[0]);
    if (status != CALL_OK) {
      goto done;
    }
    count = 1;
    if (!strcmp(mode, "expired")) {
      ok = wait_client(children[0]) &&
          receive_sentinel(endpoint.caller, endpoint.receiver);
    } else if (!strcmp(mode, "queued-timeout")) {
      if (endpoint_receive(endpoint.receiver, &packets[0]) != CALL_OK ||
          packets[0].kind != ENDPOINT_MESSAGE_SEND ||
          packets[0].size != sizeof(uint64_t) || packets[0].grant_count != 0) {
        goto done;
      }
      uint64_t deadline_ns;
      memcpy(&deadline_ns, packets[0].data, sizeof(deadline_ns));
      if (endpoint_finish(packets[0].receipt) != CALL_OK ||
          clock_sleep_until(clock, deadline_ns) != CALL_OK) {
        goto done;
      }
      ok = wait_client(children[0]) &&
          receive_sentinel(endpoint.caller, endpoint.receiver);
    } else {
      if (endpoint_receive(endpoint.receiver, &packets[0]) != CALL_OK ||
          packets[0].kind != ENDPOINT_MESSAGE_CALL || packets[0].deadline_ns == 0) {
        goto done;
      }
      if (!strcmp(mode, "deadline-reply")) {
        ok = finish_request(&packets[0], output, false);
      } else {
        bool full = !strcmp(mode, "cancel-full");
        if (full) {
          status = launch_client(launcher, image, output, memory, endpoint.caller,
              content, clock, "mixed", "2", true, &children[1]);
          if (status != CALL_OK) {
            goto done;
          }
          count = 2;
          if (!wait_client(children[1])) {
            goto done;
          }
        }
        bool notice_received = false;
        if (!full && strcmp(mode, "cancel-finish")) {
          /* RECEIVE must wake for cancellation even with no queued work. */
          notice_received = receive_cancel(endpoint.receiver, &packets[0]);
          if (!notice_received) {
            goto done;
          }
        } else if (clock_sleep_until(clock, packets[0].deadline_ns) != CALL_OK) {
          goto done;
        }
        if (!wait_client(children[0])) {
          goto done;
        }
        if (!strcmp(mode, "cancel-finish")) {
          ok = endpoint_finish(packets[0].receipt) == CALL_OK &&
              read_grants(&packets[0]) && close_grants(&packets[0]) &&
              receive_sentinel(endpoint.caller, endpoint.receiver);
        } else {
          bool occupied = !full || endpoint_send(endpoint.caller,
              NULL, 0, NULL, 0) == CALL_QUEUE_FULL;
          status = endpoint_reply(packets[0].receipt, CONTENT_OK,
              packets[0].data, packets[0].size, packets[0].grants,
              packets[0].grant_count);
          ok = occupied && status == CALL_TIMED_OUT &&
              (notice_received || receive_cancel(endpoint.receiver, &packets[0])) &&
              endpoint_finish(packets[0].receipt) == CALL_OK &&
              read_grants(&packets[0]) && close_grants(&packets[0]);
          if (ok && full) {
            for (size_t i = 0; i < ENDPOINT_DELIVERIES_MAX - 1; ++i) {
              if (endpoint_receive(endpoint.receiver, &packets[i + 1]) != CALL_OK ||
                  !finish_send(&packets[i + 1], output, i + 2)) {
                ok = false;
                break;
              }
            }
          }
        }
      }
    }
  } else if (!strcmp(mode, "send") || !strcmp(mode, "mixed")) {
    bool mixed = !strcmp(mode, "mixed");
    if (mixed) {
      status = launch_client(launcher, image, output, memory, endpoint.caller,
          content, clock, mode, "1", false, &children[0]);
      if (status != CALL_OK) {
        goto done;
      }
      count = 1;
      if (endpoint_receive(endpoint.receiver, &packets[0]) != CALL_OK ||
          packets[0].kind != ENDPOINT_MESSAGE_CALL) {
        goto done;
      }
    }
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, clock, mode, mixed ? "2" : "1", true, &children[mixed ? 1 : 0]);
    if (status != CALL_OK) {
      goto done;
    }
    count = mixed ? 2 : 1;
    if (!wait_client(children[mixed ? 1 : 0])) {
      goto done;
    }
    if (handle_close(children[mixed ? 1 : 0]) != 0) {
      goto done;
    }
    children[mixed ? 1 : 0] = HANDLE_INVALID;
    if (handle_close(endpoint.caller) != 0) {
      goto done;
    }
    endpoint.caller = HANDLE_INVALID;
    ok = true;
    size_t sends = mixed ? ENDPOINT_DELIVERIES_MAX - 1 : 1;
    for (size_t i = 0; i < sends; ++i) {
      if (endpoint_receive(endpoint.receiver, &packets[mixed ? i + 1 : 0]) != CALL_OK ||
          !finish_send(&packets[mixed ? i + 1 : 0], output, mixed ? i + 2 : 1)) {
        ok = false;
        break;
      }
    }
    if (mixed && !finish_request(&packets[0], output, false)) {
      ok = false;
    }
  } else if (!strcmp(mode, "close") || !strcmp(mode, "exit")) {
    if (endpoint_receive(endpoint.receiver, &packets[0]) != CALL_OK) {
      goto done;
    }
    if (!strcmp(mode, "exit")) {
      console_print(output, "server: exiting with a received call\n");
      /* Process teardown closes its receiver and receipt, waking the caller. */
      return 0;
    }
    if (handle_close(endpoint.receiver) != 0) {
      goto done;
    }
    endpoint.receiver = HANDLE_INVALID;
    bool receipt_closed = handle_close(packets[0].receipt) == 0;
    bool grants_closed = close_grants(&packets[0]);
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, clock, mode, "2", false, &children[1]);
    if (status != CALL_OK) {
      goto done;
    }
    ok = grants_closed && receipt_closed;
    count = 2;
  } else if (!strcmp(mode, "saturate")) {
    /* All sixteen receipts remain live, so admission of the next call fails. */
    status = launch_client(launcher, image, output, memory, endpoint.caller,
        content, clock, mode, "17", false, &children[count]);
    if (status != CALL_OK || !wait_client(children[count])) {
      goto done;
    }
    ++count;
    ok = true;
    for (size_t i = ENDPOINT_DELIVERIES_MAX; i > 0; --i) {
      if (!finish_request(&packets[i - 1], output, false)) {
        ok = false;
      }
    }
  } else {
    if (endpoint_receive(endpoint.receiver, &packets[0]) != CALL_OK) {
      goto done;
    }
    if (endpoint_receive(endpoint.receiver, &packets[1]) != CALL_OK) {
      handle_close(packets[0].receipt);
      close_grants(&packets[0]);
      goto done;
    }
    /* Both calls remain admitted while we complete the second before the first. */
    bool abandon = !strcmp(mode, "abandon");
    bool reverse_ok = finish_request(&packets[1], output,
        abandon && from_client_one(&packets[1]));
    bool first_ok = finish_request(&packets[0], output,
        abandon && from_client_one(&packets[0]));
    ok = reverse_ok && first_ok;
  }
  for (size_t i = 0; i < count; ++i) {
    if (children[i] != HANDLE_INVALID && !wait_client(children[i])) {
      ok = false;
    }
  }
  if (console_print(output, ok ? "server: complete\n" : "server: failed\n") != CALL_OK) {
    ok = false;
  }

done:
  free(packets);
  if (endpoint.caller != HANDLE_INVALID && handle_close(endpoint.caller) != 0) {
    ok = false;
  }
  if (endpoint.receiver != HANDLE_INVALID && handle_close(endpoint.receiver) != 0) {
    ok = false;
  }
  if (content != HANDLE_INVALID && handle_close(content) != 0) {
    ok = false;
  }
  if (image != HANDLE_INVALID && handle_close(image) != 0) {
    ok = false;
  }
  if (clock != HANDLE_INVALID && handle_close(clock) != 0) {
    ok = false;
  }
  for (size_t i = 0; i < ENDPOINT_DELIVERIES_MAX + 1; ++i) {
    if (children[i] != HANDLE_INVALID && handle_close(children[i]) != 0) {
      ok = false;
    }
  }
  return ok ? 0 : 1;
}
