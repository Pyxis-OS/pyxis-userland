#include <abi/endpoint.h>
#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <namespace.h>
#include <startup.h>
#include <stdio.h>
#include <string.h>
#include "../common/counter_service.h"

#define PUBLICATION_TIMEOUT_NS UINT64_C(10000000000)

static int provide(bool once)
{
  handle_t service = startup_resource("service");
  handle_t publication = startup_resource("publication");
  handle_t clock = startup_resource("clock");
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID ||
      clock == HANDLE_INVALID) {
    fputs("counter: missing provider authority\n", stderr);
    return 1;
  }
  struct endpoint_create_reply endpoint = {0};
  enum call_status status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: endpoint create failed (status %u)\n", status);
    return 1;
  }
  handle_t client = HANDLE_INVALID;
  status = endpoint_export(service, endpoint.receiver, COUNTER_FIRST_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
      HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: export failed (status %u)\n", status);
    goto done;
  }
  uint64_t now;
  if (clock_now(clock, &now) != CALL_OK || now > UINT64_MAX - PUBLICATION_TIMEOUT_NS) {
    fputs("counter: clock unavailable\n", stderr);
    status = CALL_UNAVAILABLE;
    goto done;
  }
  struct endpoint_grant grant = {client,
      COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE, HANDLE_TRANSPORT_CALL};
  struct endpoint_packet reply = {0};
  status = endpoint_request(publication, NULL, 0, &grant, 1,
      now + PUBLICATION_TIMEOUT_NS, &reply);
  handle_close(client);
  client = HANDLE_INVALID;
  if (status != CALL_OK || reply.result != CALL_OK || reply.size != 0 ||
      reply.grant_count != 0) {
    fprintf(stderr, "counter: publication failed (transport %u, result %ju)\n",
        status, (uintmax_t)reply.result);
    if (status == CALL_OK) {
      status = reply.result != CALL_OK ? reply.result : CALL_BAD_REQUEST;
    }
    goto done;
  }
  uint64_t value = 4;
  for (;;) {
    struct endpoint_packet packet = {0};
    status = endpoint_receive(endpoint.receiver, &packet);
    if (status != CALL_OK) {
      fprintf(stderr, "counter: receive failed (status %u)\n", status);
      goto done;
    }
    if (packet.kind == ENDPOINT_MESSAGE_RETIRE) {
      status = endpoint_retire_ack(endpoint.receiver, packet.object_id);
      break;
    }
    for (size_t i = 0; i < packet.grant_count; ++i) {
      handle_close(packet.grants[i].handle);
    }
    if (packet.kind == ENDPOINT_MESSAGE_CANCEL) {
      endpoint_finish(packet.receipt);
      continue;
    }
    uint64_t result = COUNTER_RESULT_BAD_REQUEST;
    if (packet.protocol == COUNTER_PROTOCOL && packet.object_id == COUNTER_FIRST_ID &&
        packet.grant_count == 0) {
      if (packet.operation == COUNTER_GET && packet.size == 0) {
        result = packet.rights & COUNTER_RIGHT_READ ? value : COUNTER_RESULT_DENIED;
      } else if (packet.operation == COUNTER_ADD && packet.size == sizeof(uint64_t)) {
        if (!(packet.rights & COUNTER_RIGHT_WRITE)) {
          result = COUNTER_RESULT_DENIED;
        } else {
          uint64_t increment;
          memcpy(&increment, packet.data, sizeof(increment));
          value += increment;
          result = value;
        }
      }
    }
    status = packet.kind == ENDPOINT_MESSAGE_CALL ?
        endpoint_reply(packet.receipt, result, NULL, 0, NULL, 0) :
        endpoint_finish(packet.receipt);
    if (status != CALL_OK) {
      fprintf(stderr, "counter: reply failed (status %u)\n", status);
      goto done;
    }
    if (once) {
      break;
    }
  }

done:
  if (client != HANDLE_INVALID) {
    handle_close(client);
  }
  handle_close(endpoint.caller);
  handle_close(endpoint.receiver);
  return status == CALL_OK ? 0 : 1;
}

static int lookup(const char *name, bool restrict_grant, bool hold, bool add)
{
  handle_t namespace_handle = startup_namespace();
  if (restrict_grant && namespace_remove(namespace_handle, name) != CALL_DENIED) {
    fputs("counter: namespace management unexpectedly available\n", stderr);
    return 1;
  }
  handle_t client = HANDLE_INVALID;
  enum call_status status = namespace_lookup(namespace_handle, name, &client);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: lookup %s failed (status %u)\n", name, status);
    return 1;
  }
  if (restrict_grant) {
    handle_t restricted;
    status = handle_copy_restricted(client, COUNTER_RIGHT_READ,
        HANDLE_TRANSPORT_CALL, &restricted);
    handle_close(client);
    if (status != CALL_OK) {
      fprintf(stderr, "counter: restrict failed (status %u)\n", status);
      return 1;
    }
    client = restricted;
  }
  handle_t clock = startup_resource("clock");
  if (hold && (clock == HANDLE_INVALID ||
      clock_sleep_for(clock, UINT64_C(30000000000)) != CALL_OK)) {
    fputs("counter: hold clock unavailable\n", stderr);
    handle_close(client);
    return 1;
  }
  struct endpoint_packet reply = {0};
  uint64_t increment = 3;
  status = endpoint_invoke(client, COUNTER_PROTOCOL,
      add ? COUNTER_ADD : COUNTER_GET,
      add ? &increment : NULL, add ? sizeof(increment) : 0,
      NULL, 0, 0, &reply);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: retained grant closed (status %u)\n", status);
  } else {
    printf("counter: %s = %ju\n", name, (uintmax_t)reply.result);
  }
  if (restrict_grant && status == CALL_OK) {
    increment = 1;
    status = endpoint_invoke(client, COUNTER_PROTOCOL, COUNTER_ADD,
        &increment, sizeof(increment), NULL, 0, 0, &reply);
    printf("counter: restricted ADD result %ju (transport %u)\n",
        (uintmax_t)reply.result, status);
    if (status == CALL_OK && reply.result != COUNTER_RESULT_DENIED) {
      status = CALL_BAD_REQUEST;
    }
  }
  handle_close(client);
  return status == CALL_OK ? 0 : 1;
}

int counter_namespace_main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--provide")) {
    return provide(false);
  }
  if (argc == 2 && !strcmp(argv[1], "--provide-once")) {
    return provide(true);
  }
  if (argc == 3 && !strcmp(argv[1], "--lookup")) {
    return lookup(argv[2], false, false, false);
  }
  if (argc == 3 && !strcmp(argv[1], "--add")) {
    return lookup(argv[2], false, false, true);
  }
  if (argc == 3 && !strcmp(argv[1], "--restrict")) {
    return lookup(argv[2], true, false, false);
  }
  if (argc == 3 && !strcmp(argv[1], "--hold")) {
    return lookup(argv[2], false, true, false);
  }
  return 1;
}
