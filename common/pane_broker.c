#include "bootstrap_environment.h"
#include "pane_broker.h"
#include <abi/launcher.h>
#include <abi/execution_group.h>
#include <abi/terminal.h>
#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <string.h>
#include <wait.h>

bool pane_broker_envelope(const struct endpoint_packet *packet)
{
  return packet->kind == ENDPOINT_MESSAGE_SEND && packet->receipt &&
      packet->delivery == ENDPOINT_DELIVERED && !packet->result &&
      !packet->deadline_ns && !packet->object_id && !packet->protocol &&
      !packet->operation && !packet->rights && !packet->reason;
}

bool pane_broker_grant(const struct endpoint_grant *grant, uint64_t protocol,
    uint64_t transport)
{
  struct handle_info info;
  return handle_query(grant->handle, &info) == CALL_OK &&
      info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      info.rights == grant->rights && info.transport == grant->transport &&
      info.transport == transport && (protocol != PROTOCOL_ENDPOINT || !info.rights);
}

void pane_broker_packet_close(struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (packet->grants[i].handle) {
      handle_close(packet->grants[i].handle);
      packet->grants[i].handle = HANDLE_INVALID;
    }
  }
  if (packet->receipt) {
    handle_close(packet->receipt);
    packet->receipt = HANDLE_INVALID;
  }
}

enum call_status pane_broker_receive(handle_t receiver, uint64_t deadline,
    struct endpoint_packet *packet)
{
  struct wait_interest interest = {.handle = receiver, .events = WAIT_READABLE};
  uint64_t events;
  enum call_status status = wait_many(&interest, 1, deadline, &events);
  if (status != CALL_OK) {
    return status;
  }
  if (events & WAIT_CLOSED) {
    return CALL_ENDPOINT_CLOSED;
  }
  if (!(events & WAIT_READABLE)) {
    return CALL_WOULD_BLOCK;
  }
  /* The sole reader sees only non-expiring SEND admissions. */
  return endpoint_receive(receiver, packet);
}

bool pane_broker_cwd_fits(uint64_t directories, uint64_t path_size)
{
  return path_size <= LAUNCH_CAPTURE_MAX_SIZE &&
      directories <= SIZE_MAX / sizeof(handle_t) &&
      directories <= (LAUNCH_CAPTURE_MAX_SIZE - path_size) /
          (sizeof(struct launch_grant) + sizeof(uint64_t));
}

static enum call_status before_deadline(handle_t clock, uint64_t deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  return status != CALL_OK ? status : now < deadline ? CALL_OK : CALL_TIMED_OUT;
}

static enum call_status exchange(handle_t caller, handle_t receiver, handle_t clock,
    const struct pane_broker_message *message, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count,
    struct endpoint_packet *packet)
{
  enum call_status status = before_deadline(clock, message->deadline);
  if (status == CALL_OK) {
    status = endpoint_send(caller, bytes, size, grants, grant_count);
  }
  if (status == CALL_OK) {
    status = pane_broker_receive(receiver, message->deadline, packet);
  }
  if (status == CALL_OK) {
    status = before_deadline(clock, message->deadline);
  }
  if (status != CALL_OK) {
    return status;
  }
  struct pane_broker_reply reply;
  if (!pane_broker_envelope(packet) || packet->size != sizeof(reply)) {
    return CALL_BAD_REQUEST;
  }
  memcpy(&reply, packet->data, sizeof(reply));
  if (reply.magic != PANE_BROKER_MAGIC || reply.operation != message->operation ||
      reply.id != message->id || reply.status >= CALL_STATUS_COUNT ||
      (reply.status != CALL_OK && packet->grant_count)) {
    return CALL_BAD_REQUEST;
  }
  return reply.status;
}

enum call_status pane_broker_start(size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, handle_t bootstrap,
    struct pane_broker_result *result)
{
  static uint64_t next_id = 1;
  *result = (struct pane_broker_result){0};
  handle_t clock = bootstrap_capability("clock"), caller = bootstrap_capability("pane_broker");
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (!next_id || now > UINT64_MAX - PANE_BROKER_TIMEOUT_NS) {
    return CALL_LIMIT;
  }
  size_t directories = cwd ? cwd->context.count : 0;
  size_t path_size = cwd && cwd->path ? strlen(cwd->path) + 1 : 0;
  if ((cwd && !directories) || !pane_broker_cwd_fits(directories, path_size)) {
    return CALL_LIMIT;
  }
  struct endpoint_create_reply endpoint = {0};
  status = endpoint_create(bootstrap_capability("service"), &endpoint);
  if (status != CALL_OK) {
    return status;
  }
  struct pane_broker_chunk chunk = {.header = {
    .magic = PANE_BROKER_MAGIC, .operation = PANE_BROKER_BEGIN, .id = next_id++,
    .deadline = now + PANE_BROKER_TIMEOUT_NS,
    .columns = columns, .rows = rows, .tab_width = tab_width,
    .directory_count = directories, .path_size = path_size,
  }};
  struct endpoint_grant grants[ENDPOINT_GRANTS_MAX] = {
    {endpoint.caller, 0, HANDLE_TRANSPORT_SEND},
    {bootstrap, 0, HANDLE_TRANSPORT_SEND},
  };
  struct endpoint_packet packet = {0};
  bool committed = false;
  status = exchange(caller, endpoint.receiver, clock, &chunk.header, &chunk.header,
      sizeof(chunk.header), grants, 2, &packet);
  handle_close(endpoint.caller);
  if (status != CALL_OK || packet.grant_count) {
    if (status == CALL_OK) {
      status = CALL_BAD_REQUEST;
    }
    goto done;
  }
  pane_broker_packet_close(&packet);
  while (chunk.header.directory_offset < directories || chunk.header.path_offset < path_size) {
    chunk.header.operation = PANE_BROKER_CHUNK;
    size_t remaining = directories - chunk.header.directory_offset;
    chunk.header.directories = remaining < ENDPOINT_GRANTS_MAX ? remaining : ENDPOINT_GRANTS_MAX;
    remaining = path_size - chunk.header.path_offset;
    chunk.header.bytes = remaining < sizeof(chunk.path) ? remaining : sizeof(chunk.path);
    for (size_t i = 0; i < chunk.header.directories; ++i) {
      grants[i].handle = cwd->context.directories[chunk.header.directory_offset + i];
      status = handle_rights(grants[i].handle, &grants[i].rights, &grants[i].transport);
      if (status != CALL_OK) {
        goto done;
      }
    }
    if (chunk.header.bytes) {
      memcpy(chunk.path, cwd->path + chunk.header.path_offset, chunk.header.bytes);
    }
    packet = (struct endpoint_packet){0};
    status = exchange(caller, endpoint.receiver, clock, &chunk.header, &chunk,
        sizeof(chunk.header) + chunk.header.bytes, grants, chunk.header.directories, &packet);
    if (status != CALL_OK || packet.grant_count) {
      if (status == CALL_OK) {
        status = CALL_BAD_REQUEST;
      }
      goto done;
    }
    pane_broker_packet_close(&packet);
    chunk.header.directory_offset += chunk.header.directories;
    chunk.header.path_offset += chunk.header.bytes;
  }
  chunk.header.operation = PANE_BROKER_COMMIT;
  chunk.header.directories = chunk.header.bytes = 0;
  packet = (struct endpoint_packet){0};
  /* Even a missing/malformed result cannot authorize a second publication. */
  committed = true;
  status = exchange(caller, endpoint.receiver, clock, &chunk.header, &chunk.header,
      sizeof(chunk.header), NULL, 0, &packet);
  if (status != CALL_OK) {
    struct pane_broker_reply failure;
    if (pane_broker_envelope(&packet) && packet.size == sizeof(failure) && !packet.grant_count) {
      memcpy(&failure, packet.data, sizeof(failure));
      if (failure.magic == PANE_BROKER_MAGIC && failure.operation == PANE_BROKER_COMMIT &&
          failure.id == chunk.header.id && failure.status == status && status != CALL_OUTCOME_UNKNOWN) {
        committed = false;
      }
    }
    goto done;
  }
  if (packet.grant_count != 3 ||
      !pane_broker_grant(&packet.grants[0], PROTOCOL_TERMINAL_ATTACHMENT, 0) ||
      !pane_broker_grant(&packet.grants[1], PROTOCOL_PROCESS, 0) ||
      !pane_broker_grant(&packet.grants[2], PROTOCOL_EXECUTION_GROUP, 0) ||
      packet.grants[0].rights != TERMINAL_RIGHTS ||
      packet.grants[1].rights != PROCESS_RIGHT_WAIT ||
      packet.grants[2].rights != (EXECUTION_GROUP_RIGHT_CONTROL | EXECUTION_GROUP_RIGHT_WAIT)) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  chunk.header.operation = PANE_BROKER_ACCEPT;
  struct endpoint_packet accepted = {0};
  status = exchange(caller, endpoint.receiver, clock, &chunk.header, &chunk.header,
      sizeof(chunk.header), NULL, 0, &accepted);
  if (status == CALL_OK && accepted.grant_count) {
    status = CALL_BAD_REQUEST;
  }
  pane_broker_packet_close(&accepted);
  if (status == CALL_OK) {
    *result = (struct pane_broker_result){packet.grants[0].handle,
        packet.grants[1].handle, packet.grants[2].handle};
    for (unsigned i = 0; i < 3; ++i) {
      packet.grants[i].handle = HANDLE_INVALID;
    }
  }

done:
  if (status != CALL_OK) {
    chunk.header.operation = PANE_BROKER_CANCEL;
    chunk.header.directories = chunk.header.bytes = 0;
    enum call_status cancelled = endpoint_send(caller, &chunk.header, sizeof(chunk.header), NULL, 0);
    if (committed || cancelled != CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
  pane_broker_packet_close(&packet);
  handle_close(endpoint.receiver);
  return status;
}
