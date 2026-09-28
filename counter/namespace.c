#include <abi/endpoint.h>
#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <namespace.h>
#include <startup.h>
#include <stdio.h>
#include <string.h>
#include "../common/counter_service.h"

#include "../common/provider_setup.h"

static int provide(bool once)
{
  handle_t service = startup_resource("service");
  handle_t publication = startup_resource("publication");
  handle_t clock = startup_resource("clock");
  struct endpoint_create_reply endpoint = {0};
  handle_t client = HANDLE_INVALID;
  enum call_status status = CALL_OK;
  enum call_status cleanup_status = CALL_OK;
  bool publication_attempted = false;
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("counter: missing provider authority\n", stderr);
    status = CALL_UNAVAILABLE;
    goto done;
  }
  status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: endpoint create failed (status %u)\n", status);
    goto done;
  }
  status = endpoint_export(service, endpoint.receiver, COUNTER_FIRST_ID,
      COUNTER_PROTOCOL, COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE,
      HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    fprintf(stderr, "counter: export failed (status %u)\n", status);
    goto done;
  }
  struct endpoint_grant grant = {client,
      COUNTER_RIGHT_READ | COUNTER_RIGHT_WRITE, HANDLE_TRANSPORT_CALL};
  status = provider_setup_report(publication, clock, CALL_OK, CALL_OK, &grant, &publication_attempted);
  if (handle_close(client) != 0) {
    cleanup_status = CALL_BAD_HANDLE;
    if (status == CALL_OK) {
      status = cleanup_status;
    }
  }
  client = HANDLE_INVALID;
  if (status != CALL_OK) {
    fprintf(stderr, "counter: publication failed (status %u)\n", status);
    goto done;
  }
  if (handle_close(publication) != 0) {
    status = CALL_BAD_HANDLE;
    publication = HANDLE_INVALID;
    goto done;
  }
  publication = HANDLE_INVALID;
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
    if (status == CALL_TIMED_OUT) {
      status = endpoint_finish(packet.receipt);
      if (status == CALL_OK) {
        continue;
      }
    }
    if (status != CALL_OK) {
      fprintf(stderr, "counter: reply failed (status %u)\n", status);
      goto done;
    }
    if (once) {
      break;
    }
  }

done:
  if (client != HANDLE_INVALID && handle_close(client) != 0) {
    cleanup_status = CALL_BAD_HANDLE;
  }
  if (endpoint.caller != HANDLE_INVALID && handle_close(endpoint.caller) != 0) {
    cleanup_status = CALL_BAD_HANDLE;
  }
  if (endpoint.receiver != HANDLE_INVALID && handle_close(endpoint.receiver) != 0) {
    cleanup_status = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK) {
    status = cleanup_status;
  }
  if (!publication_attempted && publication != HANDLE_INVALID) {
    enum call_status reported = provider_setup_report(publication, clock,
        status, cleanup_status, NULL, NULL);
    if (reported != CALL_OK) {
      fprintf(stderr, "counter: setup report failed (status %u)\n", reported);
    }
  }
  if (publication != HANDLE_INVALID && handle_close(publication) != 0 && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  return status == CALL_OK ? 0 : 1;
}

static int lookup(const char *name, bool restrict_grant, bool hold, bool add)
{
  handle_t namespace_handle = startup_namespace();
  if (restrict_grant && namespace_remove(namespace_handle, name) != CALL_DENIED) {
    fputs("counter: namespace management unexpectedly available\n", stderr);
    return 1;
  }
  if (restrict_grant) {
    fputs("counter: namespace REMOVE denied\n", stdout);
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
  if (hold) {
    printf("counter: retained %s; waiting\n", name);
    fflush(stdout);
  }
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
