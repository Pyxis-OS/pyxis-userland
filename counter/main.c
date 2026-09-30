#include <abi/console.h>
#include <abi/endpoint.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <clock.h>
#include <console.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <process.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common/counter_service.h"

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  bool ok = handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return ok;
}

static bool authority(handle_t handle, uint64_t rights, uint64_t transport)
{
  uint64_t actual_rights, actual_transport;
  return handle_rights(handle, &actual_rights, &actual_transport) == CALL_OK &&
      actual_rights == rights && actual_transport == transport;
}

static bool wait_child(handle_t child)
{
  struct process_result result;
  return process_wait(child, &result) == CALL_OK &&
      result.kind == PROCESS_EXITED && result.exit_status == 0;
}

static bool invoke_value(handle_t client, uint64_t object_id,
    uint64_t rights, uint64_t operation, uint64_t argument, uint64_t expected)
{
  struct endpoint_packet reply;
  const void *bytes = operation == COUNTER_ADD ? &argument : NULL;
  size_t size = operation == COUNTER_ADD ? sizeof(argument) : 0;
  return endpoint_invoke(client, COUNTER_PROTOCOL, operation, bytes, size,
      NULL, 0, 0, &reply) == CALL_OK && reply.result == expected &&
      reply.size == 0 && reply.grant_count == 0 &&
      reply.object_id == object_id && reply.rights == rights;
}

static int client_basic(void)
{
  handle_t first = startup_resource("first");
  handle_t second = startup_resource("second");
  handle_t output = startup_resource("output");
  handle_t read_copy = HANDLE_INVALID, send_copy = HANDLE_INVALID;
  bool ok = first != HANDLE_INVALID && second != HANDLE_INVALID &&
      authority(first, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL) &&
      authority(second, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
          HANDLE_TRANSPORT_CALL);

  if (ok) {
    ok = handle_copy_restricted(second, COUNTER_RIGHT_READ,
        HANDLE_TRANSPORT_CALL, &read_copy) == CALL_OK &&
        authority(read_copy, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL);
  }
  if (ok) {
    struct endpoint_grant grant = {read_copy, COUNTER_RIGHT_READ,
        HANDLE_TRANSPORT_CALL};
    struct endpoint_packet reply = {0};
    ok = endpoint_invoke(first, COUNTER_PROTOCOL, COUNTER_GET, NULL, 0,
        &grant, 1, 0, &reply) == CALL_OK && reply.object_id == COUNTER_FIRST_ID &&
        reply.rights == COUNTER_RIGHT_READ && reply.result == 4 &&
        reply.grant_count == 1 && reply.size == 0 &&
        authority(reply.grants[0].handle, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL);
    if (reply.grant_count == 1) {
      ok = close_handle(&reply.grants[0].handle) && ok;
    }
  }
  if (ok) {
    struct endpoint_packet reply;
    ok = endpoint_invoke(second, COUNTER_OTHER_PROTOCOL, COUNTER_GET, NULL, 0,
        NULL, 0, 0, &reply) == CALL_BAD_OPERATION &&
        reply.delivery == ENDPOINT_NOT_DELIVERED;
  }
  if (ok) {
    ok = invoke_value(read_copy, COUNTER_SECOND_ID, COUNTER_RIGHT_READ,
        COUNTER_ADD, 99, COUNTER_RESULT_DENIED);
  }
  if (ok) {
    ok = invoke_value(second, COUNTER_SECOND_ID,
        COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE, COUNTER_ADD, 3, 12);
  }
  if (ok) {
    ok = handle_copy_restricted(second, COUNTER_RIGHT_WRITE,
        HANDLE_TRANSPORT_SEND, &send_copy) == CALL_OK &&
        authority(send_copy, COUNTER_RIGHT_WRITE, HANDLE_TRANSPORT_SEND);
  }
  if (ok) {
    uint64_t increment = 2;
    struct endpoint_packet reply;
    ok = endpoint_invoke(send_copy, COUNTER_PROTOCOL, COUNTER_ADD,
        &increment, sizeof(increment), NULL, 0, 0, &reply) == CALL_DENIED &&
        reply.delivery == ENDPOINT_NOT_DELIVERED &&
        endpoint_notify(send_copy, COUNTER_PROTOCOL, COUNTER_ADD,
        &increment, sizeof(increment), NULL, 0) == CALL_OK;
  }
  bool send_closed = close_handle(&send_copy);
  bool read_closed = close_handle(&read_copy);
  bool first_closed = close_handle(&first);
  bool second_closed = close_handle(&second);
  ok = send_closed && read_closed && first_closed && second_closed && ok;
  if (output != HANDLE_INVALID) {
    console_print(output, ok ? "counter client: complete\n" : "counter client: failed\n");
  }
  return ok ? 0 : 1;
}

static int client_withdraw(void)
{
  handle_t client = startup_resource("second");
  handle_t output = startup_resource("output");
  uint64_t increment = 3;
  struct endpoint_packet reply;
  bool ok = client != HANDLE_INVALID &&
      endpoint_invoke(client, COUNTER_PROTOCOL, COUNTER_ADD, &increment,
          sizeof(increment), NULL, 0, 0, &reply) == CALL_ENDPOINT_CLOSED &&
      reply.delivery == ENDPOINT_DELIVERED;
  if (output != HANDLE_INVALID) {
    console_print(output, ok ? "withdraw client: closed after delivery\n" :
        "withdraw client: failed\n");
  }
  return close_handle(&client) && ok ? 0 : 1;
}

static int client_reuse(void)
{
  handle_t client = startup_resource("second");
  handle_t output = startup_resource("output");
  bool ok = client != HANDLE_INVALID &&
      invoke_value(client, COUNTER_SECOND_ID,
          COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE, COUNTER_GET, 0, 9);
  if (output != HANDLE_INVALID) {
    console_print(output, ok ? "reused export: complete\n" :
        "reused export: failed\n");
  }
  return close_handle(&client) && ok ? 0 : 1;
}

static int client_queued(void)
{
  handle_t client = startup_resource("second");
  handle_t output = startup_resource("output");
  uint64_t zero = 0, increment = 3;
  struct endpoint_packet reply;
  bool ok = client != HANDLE_INVALID &&
      endpoint_notify(client, COUNTER_PROTOCOL, COUNTER_ADD,
          &zero, sizeof(zero), NULL, 0) == CALL_OK &&
      endpoint_invoke(client, COUNTER_PROTOCOL, COUNTER_ADD,
          &increment, sizeof(increment), NULL, 0, 0, &reply) ==
          CALL_ENDPOINT_CLOSED && reply.delivery == ENDPOINT_NOT_DELIVERED;
  if (output != HANDLE_INVALID) {
    console_print(output, ok ? "queued client: closed before delivery\n" :
        "queued client: failed\n");
  }
  return close_handle(&client) && ok ? 0 : 1;
}

static enum call_status launch_client(handle_t launcher, handle_t image,
    handle_t memory, handle_t output, handle_t first, handle_t second,
    const char *role, handle_t *child)
{
  enum { MEMORY, OUTPUT, FIRST, SECOND };
  struct launch_grant grants[] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [OUTPUT] = {output, CONSOLE_RIGHT_WRITE, 0},
    [FIRST] = {first, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL},
    [SECOND] = {second, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
        HANDLE_TRANSPORT_CALL},
  };
  struct launch_binding resources[] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"output", OUTPUT},
    {(uintptr_t)"first", FIRST}, {(uintptr_t)"second", SECOND},
  };
  if (first == HANDLE_INVALID) {
    grants[FIRST] = grants[SECOND];
    resources[2] = (struct launch_binding){(uintptr_t)"second", FIRST};
  }
  const char *arguments[] = {"counter.pxe", role};
  struct launch_request request = {
    .image = image,
    .grants = (uintptr_t)grants,
    .grant_count = first == HANDLE_INVALID ? 3 : 4,
    .resources = (uintptr_t)resources,
    .resource_count = first == HANDLE_INVALID ? 3 : 4,
    .argv = (uintptr_t)arguments,
    .argc = 2,
  };
  return launcher_launch(launcher, &request, child);
}

static bool receive_retire(handle_t receiver, uint64_t first_id, uint64_t second_id)
{
  size_t count = second_id ? 2 : 1;
  for (size_t i = 0; i < count; ++i) {
    struct endpoint_packet notice;
    if (endpoint_receive(receiver, &notice) != CALL_OK ||
        notice.kind != ENDPOINT_MESSAGE_RETIRE ||
        (notice.object_id != first_id && notice.object_id != second_id) ||
        endpoint_retire_ack(receiver, notice.object_id) != CALL_OK) {
      return false;
    }
  }
  return true;
}

static bool serve_packet(struct endpoint_packet *packet, uint64_t *first,
    uint64_t *second)
{
  if ((packet->kind != ENDPOINT_MESSAGE_CALL &&
      packet->kind != ENDPOINT_MESSAGE_SEND) ||
      packet->protocol != COUNTER_PROTOCOL || packet->reason != 0 ||
      (packet->object_id != COUNTER_FIRST_ID &&
      packet->object_id != COUNTER_SECOND_ID)) {
    return false;
  }
  uint64_t *value = packet->object_id == COUNTER_FIRST_ID ? first : second;
  uint64_t result = COUNTER_RESULT_BAD_REQUEST;
  if (packet->operation == COUNTER_GET && (packet->rights & COUNTER_RIGHT_READ) &&
      packet->size == 0) {
    result = *value;
  } else if (packet->operation == COUNTER_ADD &&
      (packet->rights & COUNTER_RIGHT_WRITE) && packet->size == sizeof(uint64_t)) {
    uint64_t increment;
    memcpy(&increment, packet->data, sizeof(increment));
    *value += increment;
    result = *value;
  } else if ((packet->operation == COUNTER_GET && !(packet->rights & COUNTER_RIGHT_READ)) ||
      (packet->operation == COUNTER_ADD && !(packet->rights & COUNTER_RIGHT_WRITE))) {
    result = COUNTER_RESULT_DENIED;
  }
  bool grant_ok = packet->grant_count ==
      (packet->object_id == COUNTER_FIRST_ID ? 1 : 0);
  if (packet->grant_count == 1) {
    grant_ok = grant_ok && authority(packet->grants[0].handle,
        COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL);
  }
  enum call_status status;
  if (packet->kind == ENDPOINT_MESSAGE_CALL) {
    status = endpoint_reply(packet->receipt, result, NULL, 0,
        packet->grant_count ? packet->grants : NULL, packet->grant_count);
  } else {
    status = endpoint_finish(packet->receipt);
  }
  for (size_t i = 0; i < packet->grant_count; ++i) {
    grant_ok = close_handle(&packet->grants[i].handle) && grant_ok;
  }
  return grant_ok && status == CALL_OK;
}

static bool serve_call(handle_t receiver, uint64_t *first, uint64_t *second)
{
  struct endpoint_packet packet;
  return endpoint_receive(receiver, &packet) == CALL_OK &&
      serve_packet(&packet, first, second);
}

static bool serve_basic(handle_t service, handle_t receiver, handle_t launcher,
    handle_t image, handle_t memory, handle_t output)
{
  handle_t first = HANDLE_INVALID, second = HANDLE_INVALID, child = HANDLE_INVALID;
  bool ok = endpoint_export(service, receiver, COUNTER_FIRST_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL, &first) == CALL_OK &&
      endpoint_export(service, receiver, COUNTER_SECOND_ID, COUNTER_PROTOCOL,
          COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE, HANDLE_TRANSPORT_CALL,
          &second) == CALL_OK;
  if (ok) {
    ok = launch_client(launcher, image, memory, output, first, second,
        "client-basic", &child) == CALL_OK;
  }
  bool first_closed = close_handle(&first);
  bool second_closed = close_handle(&second);
  ok = first_closed && second_closed && ok;
  uint64_t first_value = 4, second_value = 9;
  size_t deliveries = 0;
  uint64_t retired = 0;
  while (ok && (deliveries < 4 || retired != 3)) {
    struct endpoint_packet packet;
    ok = endpoint_receive(receiver, &packet) == CALL_OK;
    if (!ok) {
      break;
    }
    if (packet.kind == ENDPOINT_MESSAGE_RETIRE) {
      uint64_t bit = packet.object_id == COUNTER_FIRST_ID ? 1 :
          packet.object_id == COUNTER_SECOND_ID ? 2 : 0;
      ok = bit && !(retired & bit) &&
          endpoint_retire_ack(receiver, packet.object_id) == CALL_OK;
      retired |= bit;
    } else {
      ok = deliveries < 4 && serve_packet(&packet, &first_value, &second_value);
      ++deliveries;
    }
  }
  if (ok) {
    ok = wait_child(child) && first_value == 4 && second_value == 14;
  }
  return close_handle(&child) && ok;
}

static bool serve_withdraw(handle_t service, handle_t receiver, handle_t launcher,
    handle_t image, handle_t memory, handle_t output)
{
  handle_t old = HANDLE_INVALID, fresh = HANDLE_INVALID;
  handle_t child = HANDLE_INVALID, reused_child = HANDLE_INVALID;
  bool ok = endpoint_export(service, receiver, COUNTER_SECOND_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
      HANDLE_TRANSPORT_CALL, &old) == CALL_OK;
  if (ok) {
    ok = launch_client(launcher, image, memory, output, HANDLE_INVALID, old,
        "client-withdraw", &child) == CALL_OK;
  }
  struct endpoint_packet packet;
  if (ok) {
    ok = endpoint_receive(receiver, &packet) == CALL_OK &&
        packet.kind == ENDPOINT_MESSAGE_CALL &&
        packet.object_id == COUNTER_SECOND_ID &&
        packet.protocol == COUNTER_PROTOCOL &&
        packet.operation == COUNTER_ADD &&
        packet.rights == (COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE);
  }
  if (ok) {
    ok = endpoint_withdraw(receiver, COUNTER_SECOND_ID) == CALL_OK;
  }
  if (ok) {
    struct endpoint_packet notice;
    ok = endpoint_receive(receiver, &notice) == CALL_OK &&
        notice.kind == ENDPOINT_MESSAGE_CANCEL &&
        notice.receipt == packet.receipt &&
        notice.object_id == COUNTER_SECOND_ID &&
        notice.reason == CALL_ENDPOINT_CLOSED &&
        endpoint_reply(packet.receipt, 0, NULL, 0, NULL, 0) == CALL_ENDPOINT_CLOSED &&
        endpoint_finish(packet.receipt) == CALL_OK;
  }
  if (ok) {
    ok = receive_retire(receiver, COUNTER_SECOND_ID, 0);
  }
  if (ok) {
    ok = endpoint_export(service, receiver, COUNTER_SECOND_ID,
        COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
        HANDLE_TRANSPORT_CALL, &fresh) == CALL_OK;
  }
  if (ok) {
    struct endpoint_packet reply;
    ok = endpoint_invoke(old, COUNTER_PROTOCOL, COUNTER_GET, NULL, 0,
        NULL, 0, 0, &reply) == CALL_ENDPOINT_CLOSED &&
        reply.delivery == ENDPOINT_NOT_DELIVERED;
  }
  if (ok) {
    ok = launch_client(launcher, image, memory, output, HANDLE_INVALID, fresh,
        "client-reuse", &reused_child) == CALL_OK;
  }
  bool old_closed = close_handle(&old);
  bool fresh_closed = close_handle(&fresh);
  ok = old_closed && fresh_closed && ok;
  if (ok) {
    uint64_t first_value = 4, second_value = 9;
    ok = serve_call(receiver, &first_value, &second_value) &&
        wait_child(reused_child) && wait_child(child) &&
        receive_retire(receiver, COUNTER_SECOND_ID, 0);
  }
  return close_handle(&child) && close_handle(&reused_child) && ok;
}

static bool start_exit_call(handle_t service, handle_t receiver, handle_t launcher,
    handle_t image, handle_t memory, handle_t output)
{
  handle_t client = HANDLE_INVALID, child = HANDLE_INVALID;
  if (endpoint_export(service, receiver, COUNTER_SECOND_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
      HANDLE_TRANSPORT_CALL, &client) != CALL_OK) {
    return false;
  }
  bool ok = launch_client(launcher, image, memory, output, HANDLE_INVALID,
      client, "client-withdraw", &child) == CALL_OK;
  if (ok) {
    struct endpoint_packet packet;
    ok = endpoint_receive(receiver, &packet) == CALL_OK &&
        packet.kind == ENDPOINT_MESSAGE_CALL &&
        packet.object_id == COUNTER_SECOND_ID;
  }
  if (!ok) {
    close_handle(&child);
    close_handle(&client);
  }
  /* Success leaves the receiver and live receipt to owner-process teardown. */
  return ok;
}

static bool serve_queued_withdraw(handle_t service, handle_t receiver,
    handle_t launcher, handle_t image, handle_t memory, handle_t output,
    handle_t clock)
{
  handle_t client = HANDLE_INVALID, child = HANDLE_INVALID;
  bool ok = endpoint_export(service, receiver, COUNTER_SECOND_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
      HANDLE_TRANSPORT_CALL, &client) == CALL_OK;
  if (ok) {
    ok = launch_client(launcher, image, memory, output, HANDLE_INVALID,
        client, "client-queued", &child) == CALL_OK;
  }
  if (ok) {
    uint64_t first_value = 4, second_value = 9;
    struct endpoint_packet marker;
    ok = endpoint_receive(receiver, &marker) == CALL_OK &&
        marker.kind == ENDPOINT_MESSAGE_SEND &&
        marker.object_id == COUNTER_SECOND_ID &&
        marker.operation == COUNTER_ADD &&
        serve_packet(&marker, &first_value, &second_value) &&
        second_value == 9;
  }
  /* The first SEND shows the child reached the export. Parking this process
   * gives the child time to admit its following CALL before withdrawal. */
  if (ok) {
    ok = clock_sleep_for(clock, UINT64_C(1000000000)) == CALL_OK &&
        endpoint_withdraw(receiver, COUNTER_SECOND_ID) == CALL_OK;
  }
  if (ok) {
    ok = wait_child(child) && receive_retire(receiver, COUNTER_SECOND_ID, 0);
  }
  bool client_closed = close_handle(&client);
  bool child_closed = close_handle(&child);
  return ok && client_closed && child_closed;
}

static bool serve_retire_full(handle_t service, handle_t receiver, handle_t caller)
{
  handle_t client = HANDLE_INVALID;
  bool ok = endpoint_export(service, receiver, COUNTER_FIRST_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ, HANDLE_TRANSPORT_CALL,
      &client) == CALL_OK;
  for (size_t i = 0; ok && i < ENDPOINT_DELIVERIES_MAX; ++i) {
    ok = endpoint_send(caller, NULL, 0, NULL, 0) == CALL_OK;
  }
  if (ok) {
    ok = endpoint_send(caller, NULL, 0, NULL, 0) == CALL_QUEUE_FULL;
  }
  if (ok) {
    ok = endpoint_withdraw(receiver, COUNTER_FIRST_ID) == CALL_OK;
  }
  ok = close_handle(&client) && ok;
  if (ok) {
    struct endpoint_packet notice;
    ok = endpoint_receive(receiver, &notice) == CALL_OK &&
        notice.kind == ENDPOINT_MESSAGE_RETIRE &&
        notice.object_id == COUNTER_FIRST_ID &&
        endpoint_send(caller, NULL, 0, NULL, 0) == CALL_QUEUE_FULL &&
        endpoint_retire_ack(receiver, COUNTER_FIRST_ID) == CALL_OK;
  }
  for (size_t i = 0; ok && i < ENDPOINT_DELIVERIES_MAX; ++i) {
    struct endpoint_packet packet;
    ok = endpoint_receive(receiver, &packet) == CALL_OK &&
        packet.kind == ENDPOINT_MESSAGE_SEND && packet.object_id == 0 &&
        packet.protocol == 0 && packet.operation == 0 &&
        packet.rights == 0 && packet.size == 0 && packet.grant_count == 0 &&
        endpoint_finish(packet.receipt) == CALL_OK;
  }
  return ok;
}

int main(int argc, char **argv)
{
  if (argc > 1 && (!strcmp(argv[1], "--provide") ||
      !strcmp(argv[1], "--provide-once") || !strcmp(argv[1], "--lookup") ||
      !strcmp(argv[1], "--add") || !strcmp(argv[1], "--restrict") ||
      !strcmp(argv[1], "--hold"))) {
    extern int counter_namespace_main(int argc, char **argv);
    return counter_namespace_main(argc, argv);
  }
  if (argc == 2 && !strcmp(argv[1], "client-basic")) {
    return client_basic();
  }
  if (argc == 2 && !strcmp(argv[1], "client-withdraw")) {
    return client_withdraw();
  }
  if (argc == 2 && !strcmp(argv[1], "client-reuse")) {
    return client_reuse();
  }
  if (argc == 2 && !strcmp(argv[1], "client-queued")) {
    return client_queued();
  }
  if (argc > 2 || (argc == 2 && strcmp(argv[1], "--withdraw") &&
      strcmp(argv[1], "--queued-withdraw") &&
      strcmp(argv[1], "--retire-full") && strcmp(argv[1], "--exit"))) {
    return 1;
  }
  handle_t service = startup_resource("service");
  handle_t launcher = startup_resource("launcher");
  handle_t memory = startup_resource("memory");
  handle_t output = startup_resource("output");
  handle_t clock = startup_resource("clock");
  handle_t app = startup_root("app");
  if (service == HANDLE_INVALID || launcher == HANDLE_INVALID ||
      memory == HANDLE_INVALID || output == HANDLE_INVALID ||
      (argc == 2 && !strcmp(argv[1], "--queued-withdraw") &&
      clock == HANDLE_INVALID) ||
      app == HANDLE_INVALID) {
    return 1;
  }
  handle_t image = HANDLE_INVALID;
  if (directory_lookup(app, "counter.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image) != CALL_OK) {
    return 1;
  }
  struct endpoint_create_reply endpoint;
  if (endpoint_create(service, &endpoint) != CALL_OK) {
    close_handle(&image);
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "--exit")) {
    bool launched = start_exit_call(service, endpoint.receiver, launcher,
        image, memory, output);
    console_print(output, launched ? "counter provider: exiting with call\n" :
        "counter provider: failed\n");
    return launched ? 0 : 1;
  }
  bool ok;
  if (argc == 2 && !strcmp(argv[1], "--withdraw")) {
    ok = serve_withdraw(service, endpoint.receiver, launcher, image, memory, output);
  } else if (argc == 2 && !strcmp(argv[1], "--queued-withdraw")) {
    ok = serve_queued_withdraw(service, endpoint.receiver, launcher, image,
        memory, output, clock);
  } else if (argc == 2 && !strcmp(argv[1], "--retire-full")) {
    ok = serve_retire_full(service, endpoint.receiver, endpoint.caller);
  } else {
    ok = serve_basic(service, endpoint.receiver, launcher, image, memory, output);
  }
  ok = close_handle(&endpoint.caller) && close_handle(&endpoint.receiver) &&
      close_handle(&image) && ok;
  console_print(output, ok ? "counter provider: complete\n" :
      "counter provider: failed\n");
  return ok ? 0 : 1;
}
