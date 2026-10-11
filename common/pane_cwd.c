#include "pane_cwd.h"
#include <abi/launcher.h>
#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>
#include <wait.h>

#define PANE_CWD_MAGIC UINT64_C(0x707978637764)
#define PANE_CWD_TIMEOUT_NS UINT64_C(1000000000)

enum pane_cwd_operation {
  PANE_CWD_ANNOUNCE = 1,
  PANE_CWD_BEGIN,
  PANE_CWD_READ,
  PANE_CWD_DONE,
};

struct pane_cwd_request {
  uint64_t magic;
  uint64_t operation;
  uint64_t token;
  uint64_t deadline;
  uint64_t directory_offset;
  uint64_t path_offset;
};

struct pane_cwd_reply {
  uint64_t magic;
  uint64_t operation;
  uint64_t token;
  uint64_t status;
  uint64_t directory_count;
  uint64_t path_size;
  uint64_t directory_offset;
  uint64_t path_offset;
  uint64_t directories;
  uint64_t bytes;
};

struct pane_cwd_chunk {
  struct pane_cwd_reply header;
  char path[ENDPOINT_DATA_MAX - sizeof(struct pane_cwd_reply)];
};

static void close_handle(handle_t *handle)
{
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  if (owned != HANDLE_INVALID) {
    handle_close(owned);
  }
}

static void close_packet(struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    close_handle(&packet->grants[i].handle);
  }
  /* Each SEND receipt is finished once, including malformed messages. */
  close_handle(&packet->receipt);
}

static bool send_envelope(const struct endpoint_packet *packet)
{
  return packet->kind == ENDPOINT_MESSAGE_SEND &&
      packet->receipt != HANDLE_INVALID && packet->delivery == ENDPOINT_DELIVERED &&
      !packet->result && !packet->deadline_ns && !packet->object_id &&
      !packet->protocol && !packet->operation && !packet->rights && !packet->reason;
}

static bool grant_matches(const struct endpoint_grant *grant, uint64_t protocol,
    uint64_t transport)
{
  struct handle_info info;
  return handle_query(grant->handle, &info) == CALL_OK &&
      info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      info.rights == grant->rights && info.transport == grant->transport &&
      info.transport == transport && (protocol != PROTOCOL_ENDPOINT || !info.rights);
}

static enum call_status receive_ready(handle_t receiver, uint64_t deadline,
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
  /* SEND cannot expire or be withdrawn while this sole reader runs. */
  return endpoint_receive(receiver, packet);
}

static enum call_status before_deadline(handle_t clock, uint64_t deadline)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  return status != CALL_OK ? status : now < deadline ? CALL_OK : CALL_TIMED_OUT;
}

static bool snapshot_fits(uint64_t directories, uint64_t path_size)
{
  uint64_t directory_bytes = sizeof(struct launch_grant) + sizeof(uint64_t);
  return directories && path_size <= LAUNCH_CAPTURE_MAX_SIZE &&
      directories <= SIZE_MAX / sizeof(handle_t) &&
      directories <= (LAUNCH_CAPTURE_MAX_SIZE - path_size) / directory_bytes;
}

enum call_status pane_cwd_provider_init(struct pane_cwd_provider *provider,
    handle_t service, handle_t bootstrap, handle_t clock)
{
  if (!provider) {
    return CALL_BAD_REQUEST;
  }
  *provider = (struct pane_cwd_provider){0};
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  struct endpoint_create_reply endpoint = {0};
  if (status == CALL_OK) {
    status = endpoint_create(service, &endpoint);
  }
  if (status == CALL_OK) {
    struct pane_cwd_request announce = {
      .magic = PANE_CWD_MAGIC, .operation = PANE_CWD_ANNOUNCE,
    };
    struct endpoint_grant grant = {
      .handle = endpoint.caller, .transport = HANDLE_TRANSPORT_SEND,
    };
    status = endpoint_send(bootstrap, &announce, sizeof(announce), &grant, 1);
  }
  close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    close_handle(&endpoint.receiver);
    return status;
  }
  *provider = (struct pane_cwd_provider){endpoint.receiver, clock, 1};
  return CALL_OK;
}

void pane_cwd_provider_close(struct pane_cwd_provider *provider)
{
  if (provider) {
    close_handle(&provider->receiver);
    *provider = (struct pane_cwd_provider){0};
  }
}

bool pane_cwd_receive_service(handle_t bootstrap_receiver, handle_t *caller)
{
  if (!caller) {
    return false;
  }
  *caller = HANDLE_INVALID;
  struct endpoint_packet packet = {0};
  if (receive_ready(bootstrap_receiver, 0, &packet) != CALL_OK) {
    return false;
  }
  struct pane_cwd_request announce;
  bool valid = send_envelope(&packet) && packet.size == sizeof(announce) &&
      packet.grant_count == 1;
  if (valid) {
    memcpy(&announce, packet.data, sizeof(announce));
    valid = announce.magic == PANE_CWD_MAGIC && announce.operation == PANE_CWD_ANNOUNCE &&
        !announce.token && !announce.deadline && !announce.directory_offset &&
        !announce.path_offset && grant_matches(&packet.grants[0],
        PROTOCOL_ENDPOINT, HANDLE_TRANSPORT_SEND);
  }
  if (valid) {
    *caller = packet.grants[0].handle;
    packet.grants[0].handle = HANDLE_INVALID;
  }
  close_packet(&packet);
  return valid;
}

static bool read_request(const struct endpoint_packet *packet,
    struct pane_cwd_request *request, uint64_t grants)
{
  if (!send_envelope(packet) || packet->size != sizeof(*request) ||
      packet->grant_count != grants) {
    return false;
  }
  memcpy(request, packet->data, sizeof(*request));
  return request->magic == PANE_CWD_MAGIC;
}

void pane_cwd_provider_serve(void *context)
{
  struct pane_cwd_provider *provider = context;
  if (!provider || provider->receiver == HANDLE_INVALID) {
    return;
  }
  struct endpoint_packet packet = {0};
  enum call_status ready = receive_ready(provider->receiver, 0, &packet);
  if (ready != CALL_OK) {
    if (ready != CALL_WOULD_BLOCK) {
      pane_cwd_provider_close(provider);
    }
    return;
  }
  struct pane_cwd_request begin;
  bool valid = read_request(&packet, &begin, 1) && begin.operation == PANE_CWD_BEGIN &&
      !begin.token && !begin.directory_offset && !begin.path_offset && begin.deadline &&
      grant_matches(&packet.grants[0], PROTOCOL_ENDPOINT, HANDLE_TRANSPORT_SEND);
  handle_t reply_caller = HANDLE_INVALID;
  if (valid) {
    reply_caller = packet.grants[0].handle;
    packet.grants[0].handle = HANDLE_INVALID;
  }
  close_packet(&packet);
  if (!valid) {
    return;
  }
  uint64_t now, deadline = begin.deadline;
  enum call_status status = clock_now(provider->clock, &now);
  if (status != CALL_OK || now >= deadline || now > UINT64_MAX - PANE_CWD_TIMEOUT_NS) {
    close_handle(&reply_caller);
    return;
  }
  if (deadline > now + PANE_CWD_TIMEOUT_NS) {
    deadline = now + PANE_CWD_TIMEOUT_NS;
  }
  struct pyxis_working_snapshot snapshot = {0};
  struct pane_cwd_chunk chunk = {.header = {
    .magic = PANE_CWD_MAGIC, .operation = PANE_CWD_BEGIN,
    .token = provider->next_token,
  }};
  if (!provider->next_token) {
    status = CALL_LIMIT;
  } else {
    /* Exhaustion stays zero permanently; a token is never reused. */
    ++provider->next_token;
    status = pyxis_working_snapshot_init(&snapshot, NULL);
  }
  size_t path_size = snapshot.path ? strlen(snapshot.path) + 1 : 0;
  if (status == CALL_OK && !snapshot.context.count) {
    status = CALL_UNAVAILABLE;
  }
  if (status == CALL_OK && !snapshot_fits(snapshot.context.count, path_size)) {
    status = CALL_LIMIT;
  }
  chunk.header.status = status;
  if (status == CALL_OK) {
    chunk.header.directory_count = snapshot.context.count;
    chunk.header.path_size = path_size;
  }
  if (before_deadline(provider->clock, deadline) != CALL_OK ||
      endpoint_send(reply_caller, &chunk.header, sizeof(chunk.header), NULL, 0) != CALL_OK ||
      status != CALL_OK) {
    goto done;
  }
  size_t directories = 0, bytes = 0;
  for (;;) {
    packet = (struct endpoint_packet){0};
    if (before_deadline(provider->clock, deadline) != CALL_OK ||
        receive_ready(provider->receiver, deadline, &packet) != CALL_OK) {
      break;
    }
    if (before_deadline(provider->clock, deadline) != CALL_OK) {
      close_packet(&packet);
      break;
    }
    struct pane_cwd_request request;
    valid = read_request(&packet, &request, 0);
    close_packet(&packet);
    if (!valid) {
      break;
    }
    /* A timed-out client's late SEND is still admitted. It cannot advance or
     * finish this transaction, nor attach authority to a later snapshot. */
    if (request.token != chunk.header.token) {
      continue;
    }
    if (request.deadline != begin.deadline || request.directory_offset != directories ||
        request.path_offset != bytes ||
        (request.operation != PANE_CWD_READ && request.operation != PANE_CWD_DONE)) {
      break;
    }
    chunk.header.operation = request.operation;
    chunk.header.directory_offset = directories;
    chunk.header.path_offset = bytes;
    chunk.header.directories = 0;
    chunk.header.bytes = 0;
    struct endpoint_grant grants[ENDPOINT_GRANTS_MAX] = {0};
    if (request.operation == PANE_CWD_DONE) {
      if (directories != snapshot.context.count || bytes != path_size) {
        break;
      }
    } else {
      if (directories == snapshot.context.count && bytes == path_size) {
        break;
      }
      size_t count = snapshot.context.count - directories;
      chunk.header.directories = count < ENDPOINT_GRANTS_MAX ? count : ENDPOINT_GRANTS_MAX;
      count = path_size - bytes;
      chunk.header.bytes = count < sizeof(chunk.path) ? count : sizeof(chunk.path);
      for (size_t i = 0; i < chunk.header.directories; ++i) {
        grants[i].handle = snapshot.context.directories[directories + i];
        status = handle_rights(grants[i].handle, &grants[i].rights, &grants[i].transport);
        if (status != CALL_OK) {
          goto done;
        }
      }
      if (chunk.header.bytes) {
        memcpy(chunk.path, snapshot.path + bytes, chunk.header.bytes);
      }
    }
    if (before_deadline(provider->clock, deadline) != CALL_OK ||
        endpoint_send(reply_caller, &chunk, sizeof(chunk.header) + chunk.header.bytes,
        grants, chunk.header.directories) != CALL_OK) {
      break;
    }
    if (request.operation == PANE_CWD_DONE) {
      break;
    }
    directories += chunk.header.directories;
    bytes += chunk.header.bytes;
  }
done:
  pyxis_working_snapshot_close(&snapshot);
  close_handle(&reply_caller);
}

static enum call_status exchange(handle_t caller, handle_t receiver, handle_t clock,
    const struct pane_cwd_request *request, const struct endpoint_grant *grant,
    struct endpoint_packet *packet, struct pane_cwd_reply *reply)
{
  enum call_status status = before_deadline(clock, request->deadline);
  if (status == CALL_OK) {
    status = endpoint_send(caller, request, sizeof(*request), grant, grant ? 1 : 0);
  }
  if (status == CALL_OK) {
    status = receive_ready(receiver, request->deadline, packet);
  }
  if (status == CALL_OK) {
    status = before_deadline(clock, request->deadline);
  }
  if (status != CALL_OK) {
    return status;
  }
  if (!send_envelope(packet) || packet->size < sizeof(*reply)) {
    return CALL_BAD_REQUEST;
  }
  memcpy(reply, packet->data, sizeof(*reply));
  if (reply->magic != PANE_CWD_MAGIC || reply->operation != request->operation ||
      !reply->token || (request->token && reply->token != request->token) ||
      reply->status >= CALL_STATUS_COUNT ||
      packet->size != sizeof(*reply) + reply->bytes ||
      reply->bytes > ENDPOINT_DATA_MAX - sizeof(*reply) ||
      reply->directories != packet->grant_count ||
      reply->directory_offset != request->directory_offset ||
      reply->path_offset != request->path_offset) {
    return CALL_BAD_REQUEST;
  }
  if (reply->status != CALL_OK && (reply->directory_count || reply->path_size ||
      reply->directories || reply->bytes)) {
    return CALL_BAD_REQUEST;
  }
  return reply->status;
}

enum call_status pane_cwd_snapshot(handle_t service, handle_t caller,
    handle_t clock, struct pyxis_working_snapshot *out)
{
  if (!out) {
    return CALL_BAD_REQUEST;
  }
  *out = (struct pyxis_working_snapshot){0};
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (now > UINT64_MAX - PANE_CWD_TIMEOUT_NS) {
    return CALL_LIMIT;
  }
  struct endpoint_create_reply endpoint = {0};
  status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    return status;
  }
  struct pane_cwd_request request = {
    .magic = PANE_CWD_MAGIC, .operation = PANE_CWD_BEGIN,
    .deadline = now + PANE_CWD_TIMEOUT_NS,
  };
  struct endpoint_grant grant = {
    .handle = endpoint.caller, .transport = HANDLE_TRANSPORT_SEND,
  };
  struct endpoint_packet packet = {0};
  struct pane_cwd_reply reply;
  status = exchange(caller, endpoint.receiver, clock, &request, &grant, &packet, &reply);
  close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    goto done;
  }
  if (reply.directories || reply.bytes || !snapshot_fits(reply.directory_count, reply.path_size)) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  size_t directory_count = reply.directory_count, path_size = reply.path_size;
  request.token = reply.token;
  out->context.directories = calloc(directory_count, sizeof(handle_t));
  out->context.capacity = directory_count;
  out->path = path_size ? malloc(path_size) : NULL;
  if (!out->context.directories || (path_size && !out->path)) {
    status = CALL_NO_MEMORY;
    goto done;
  }
  close_packet(&packet);
  while (request.directory_offset < directory_count || request.path_offset < path_size) {
    packet = (struct endpoint_packet){0};
    request.operation = PANE_CWD_READ;
    status = exchange(caller, endpoint.receiver, clock, &request, NULL, &packet, &reply);
    if (status != CALL_OK) {
      goto done;
    }
    if (reply.directory_count != directory_count || reply.path_size != path_size ||
        reply.directories > directory_count - request.directory_offset ||
        reply.bytes > path_size - request.path_offset || (!reply.directories && !reply.bytes)) {
      status = CALL_BAD_REQUEST;
      goto done;
    }
    for (size_t i = 0; i < reply.directories; ++i) {
      if (!grant_matches(&packet.grants[i], PROTOCOL_DIRECTORY, 0)) {
        status = CALL_WRONG_TYPE;
        goto done;
      }
      out->context.directories[out->context.count++] = packet.grants[i].handle;
      packet.grants[i].handle = HANDLE_INVALID;
    }
    if (reply.bytes) {
      memcpy(out->path + request.path_offset, packet.data + sizeof(reply), reply.bytes);
    }
    request.directory_offset += reply.directories;
    request.path_offset += reply.bytes;
    close_packet(&packet);
  }
  if (path_size && (out->path[path_size - 1] || memchr(out->path, 0, path_size - 1))) {
    status = CALL_BAD_REQUEST;
    goto done;
  }
  packet = (struct endpoint_packet){0};
  request.operation = PANE_CWD_DONE;
  status = exchange(caller, endpoint.receiver, clock, &request, NULL, &packet, &reply);
  if (status == CALL_OK && (reply.directories || reply.bytes ||
      reply.directory_count != directory_count || reply.path_size != path_size)) {
    status = CALL_BAD_REQUEST;
  }
done:
  close_packet(&packet);
  close_handle(&endpoint.receiver);
  close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    pyxis_working_snapshot_close(out);
  }
  return status;
}
