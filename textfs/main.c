#include <abi/file.h>
#include <abi/provider.h>
#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "guide.h"

#define SERVICE_ID UINT64_C(1)
#define FILE_FIRST_ID UINT64_C(2)
#define FILE_SLOTS (ENDPOINT_EXPORTS_MAX - 1)
#define PUBLICATION_TIMEOUT_NS UINT64_C(10000000000)

static const char default_welcome[] =
    "Welcome to Pyxis. This immutable file comes from a userspace provider.\n"
    "Read text://guide for file, namespace and service commands.\n";

struct file_export {
  const char *bytes;
  size_t size;
  /* A slot remains reserved until its export's RETIRE is acknowledged. */
  bool live;
};

struct provider {
  handle_t service;
  handle_t receiver;
  const char *welcome;
  bool published;
  struct file_export files[FILE_SLOTS];
};

static enum call_status close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return CALL_OK;
  }
  enum call_status status = handle_close(*handle);
  *handle = HANDLE_INVALID;
  return status;
}

static enum call_status close_grants(struct endpoint_packet *packet)
{
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    enum call_status closed = close_handle(&packet->grants[i].handle);
    if (closed != CALL_OK) {
      status = closed;
    }
  }
  return status;
}

static enum call_status reply(struct endpoint_packet *packet, uint64_t result,
    const void *bytes, size_t size, const struct endpoint_grant *grant)
{
  enum call_status status = endpoint_reply(packet->receipt, result, bytes, size,
      grant, grant ? 1 : 0);
  if (status == CALL_OK) {
    return CALL_OK;
  }
  enum call_status finished = endpoint_finish(packet->receipt);
  if (finished != CALL_OK) {
    return finished;
  }
  if (status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED) {
    return CALL_OK;
  }
  return status;
}

static bool scheme_letter(uint8_t byte)
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

static bool uri_name(const uint8_t *uri, size_t size, const char *name)
{
  if (size == 0 || !scheme_letter(uri[0])) {
    return false;
  }
  size_t separator = 1;
  while (separator < size && uri[separator] != ':') {
    uint8_t byte = uri[separator];
    if (!scheme_letter(byte) && !(byte >= '0' && byte <= '9') &&
        byte != '+' && byte != '-' && byte != '.') {
      return false;
    }
    ++separator;
  }
  size_t name_size = strlen(name);
  return separator < size && size - separator == 3 + name_size &&
      uri[separator + 1] == '/' && uri[separator + 2] == '/' &&
      memcmp(uri + separator + 3, name, name_size) == 0;
}

static enum call_status open_file(struct provider *provider,
    struct endpoint_packet *packet)
{
  if (packet->operation != PROVIDER_OPEN) {
    return reply(packet, CALL_BAD_OPERATION, NULL, 0, NULL);
  }
  if (packet->size < sizeof(struct provider_open_request)) {
    return reply(packet, CALL_BAD_REQUEST, NULL, 0, NULL);
  }
  struct provider_open_request request;
  memcpy(&request, packet->data, sizeof(request));
  if (request.rights == 0 || (request.rights & ~FILE_RIGHTS) ||
      request.uri_size == 0 || request.uri_size > PROVIDER_URI_MAX_BYTES ||
      packet->size - sizeof(request) != request.uri_size) {
    return reply(packet, CALL_BAD_REQUEST, NULL, 0, NULL);
  }
  if (((request.rights & FILE_RIGHT_READ) &&
      !(packet->rights & PROVIDER_RIGHT_OPEN_READ)) ||
      ((request.rights & FILE_RIGHT_WRITE) &&
      !(packet->rights & PROVIDER_RIGHT_OPEN_WRITE))) {
    return reply(packet, CALL_DENIED, NULL, 0, NULL);
  }
  if (request.rights & FILE_RIGHT_WRITE) {
    return reply(packet, CALL_READ_ONLY, NULL, 0, NULL);
  }
  const uint8_t *uri = packet->data + sizeof(request);
  const char *bytes;
  size_t size;
  if (uri_name(uri, request.uri_size, "welcome")) {
    bytes = provider->welcome;
    size = strlen(bytes);
  } else if (uri_name(uri, request.uri_size, "guide")) {
    bytes = guide;
    size = sizeof(guide) - 1;
  } else {
    return reply(packet, CALL_NOT_FOUND, NULL, 0, NULL);
  }
  size_t slot = 0;
  while (slot < FILE_SLOTS && provider->files[slot].live) {
    ++slot;
  }
  if (slot == FILE_SLOTS) {
    return reply(packet, CALL_LIMIT, NULL, 0, NULL);
  }
  handle_t client = HANDLE_INVALID;
  enum call_status status = endpoint_export(provider->service, provider->receiver,
      FILE_FIRST_ID + slot, PROTOCOL_FILE, FILE_RIGHT_READ,
      HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    if (status == CALL_OUTCOME_UNKNOWN) {
      endpoint_finish(packet->receipt);
      return status;
    }
    return reply(packet, status, NULL, 0, NULL);
  }
  provider->files[slot] = (struct file_export){bytes, size, true};
  static const char media_type[] = "text/plain; charset=utf-8";
  struct provider_open_reply header = {
    .protocol = PROTOCOL_FILE,
    .representation = PROVIDER_REPRESENTATION_BYTES,
    .media_type_size = sizeof(media_type) - 1,
  };
  uint8_t response[sizeof(header) + sizeof(media_type) - 1];
  memcpy(response, &header, sizeof(header));
  memcpy(response + sizeof(header), media_type, sizeof(media_type) - 1);
  struct endpoint_grant grant = {client, request.rights, HANDLE_TRANSPORT_CALL};
  status = endpoint_reply(packet->receipt, CALL_OK, response, sizeof(response),
      &grant, 1);
  /* The export state survives until RETIRE, including when reply transfer fails. */
  enum call_status withdrawn = CALL_OK;
  if (status != CALL_OK) {
    withdrawn = endpoint_withdraw(provider->receiver, FILE_FIRST_ID + slot);
  }
  enum call_status closed = close_handle(&client);
  enum call_status finished = CALL_OK;
  if (status != CALL_OK) {
    finished = endpoint_finish(packet->receipt);
  }
  if (withdrawn != CALL_OK) {
    return withdrawn;
  }
  if (closed != CALL_OK) {
    return closed;
  }
  if (finished != CALL_OK) {
    return finished;
  }
  return status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ? CALL_OK : status;
}

static enum call_status serve_file(struct file_export *file,
    struct endpoint_packet *packet)
{
  if (packet->operation == FILE_WRITE || packet->operation == FILE_RESIZE ||
      packet->operation == FILE_SYNC) {
    return reply(packet, CALL_DENIED, NULL, 0, NULL);
  }
  if (packet->operation != FILE_READ && packet->operation != FILE_SIZE) {
    return reply(packet, CALL_BAD_OPERATION, NULL, 0, NULL);
  }
  if (!(packet->rights & FILE_RIGHT_READ)) {
    return reply(packet, CALL_DENIED, NULL, 0, NULL);
  }
  if (packet->operation == FILE_SIZE) {
    if (packet->size != 0) {
      return reply(packet, CALL_BAD_REQUEST, NULL, 0, NULL);
    }
    struct file_size_reply response = {file->size};
    return reply(packet, CALL_OK, &response, sizeof(response), NULL);
  }
  if (packet->size != sizeof(struct file_read_request)) {
    return reply(packet, CALL_BAD_REQUEST, NULL, 0, NULL);
  }
  struct file_read_request request;
  memcpy(&request, packet->data, sizeof(request));
  if (request.capacity > FILE_READ_MAX_BYTES) {
    return reply(packet, CALL_BAD_REQUEST, NULL, 0, NULL);
  }
  size_t read = 0;
  if (request.offset < file->size) {
    read = file->size - request.offset;
    if (read > request.capacity) {
      read = request.capacity;
    }
  }
  uint8_t response[FILE_PAYLOAD_MAX];
  struct file_read_reply header = {read};
  memcpy(response, &header, sizeof(header));
  if (read != 0) {
    memcpy(response + sizeof(header), file->bytes + request.offset, read);
  }
  return reply(packet, CALL_OK, response, sizeof(header) + read, NULL);
}

static enum call_status serve(struct provider *provider)
{
  for (;;) {
    struct endpoint_packet packet = {0};
    enum call_status status = endpoint_receive(provider->receiver, &packet);
    if (status != CALL_OK) {
      return status;
    }
    if (packet.kind == ENDPOINT_MESSAGE_RETIRE) {
      if (packet.object_id == SERVICE_ID && packet.protocol == PROTOCOL_PROVIDER &&
          provider->published) {
        provider->published = false;
      } else if (packet.object_id >= FILE_FIRST_ID &&
          packet.object_id - FILE_FIRST_ID < FILE_SLOTS &&
          packet.protocol == PROTOCOL_FILE &&
          provider->files[packet.object_id - FILE_FIRST_ID].live) {
        provider->files[packet.object_id - FILE_FIRST_ID] = (struct file_export){0};
      } else {
        return CALL_BAD_REQUEST;
      }
      status = endpoint_retire_ack(provider->receiver, packet.object_id);
      if (status != CALL_OK) {
        return status;
      }
      bool live = provider->published;
      for (size_t i = 0; i < FILE_SLOTS; ++i) {
        live = live || provider->files[i].live;
      }
      if (!live) {
        return CALL_OK;
      }
      continue;
    }
    status = close_grants(&packet);
    if (status != CALL_OK) {
      endpoint_finish(packet.receipt);
      return status;
    }
    if (packet.kind == ENDPOINT_MESSAGE_CANCEL || packet.kind == ENDPOINT_MESSAGE_SEND) {
      status = endpoint_finish(packet.receipt);
    } else if (packet.grant_count != 0 || packet.reason != 0) {
      status = reply(&packet, CALL_BAD_REQUEST, NULL, 0, NULL);
    } else if (packet.object_id == SERVICE_ID && packet.protocol == PROTOCOL_PROVIDER &&
        provider->published) {
      status = open_file(provider, &packet);
    } else if (packet.object_id >= FILE_FIRST_ID &&
        packet.object_id - FILE_FIRST_ID < FILE_SLOTS && packet.protocol == PROTOCOL_FILE &&
        provider->files[packet.object_id - FILE_FIRST_ID].live) {
      status = serve_file(&provider->files[packet.object_id - FILE_FIRST_ID], &packet);
    } else {
      status = reply(&packet, CALL_BAD_REQUEST, NULL, 0, NULL);
    }
    if (status != CALL_OK) {
      return status;
    }
  }
}

int main(int argc, char **argv)
{
  const char *welcome = default_welcome;
  if (argc == 3 && !strcmp(argv[1], "--welcome")) {
    welcome = argv[2];
  } else if (argc != 1) {
    fputs("usage: textfs [--welcome MESSAGE]\n", stderr);
    return 1;
  }
  handle_t service = startup_resource("service");
  handle_t publication = startup_resource("publication");
  handle_t clock = startup_resource("clock");
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("textfs: missing provider authority\n", stderr);
    return 1;
  }
  struct endpoint_create_reply endpoint = {0};
  enum call_status status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "textfs: create failed (status %u)\n", status);
    return 1;
  }
  handle_t client = HANDLE_INVALID;
  struct provider provider = {
    .service = service,
    .receiver = endpoint.receiver,
    .welcome = welcome,
  };
  status = close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    goto done;
  }
  status = endpoint_export(service, endpoint.receiver, SERVICE_ID, PROTOCOL_PROVIDER,
      PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    goto done;
  }
  uint64_t now;
  status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - PUBLICATION_TIMEOUT_NS) {
    status = CALL_UNAVAILABLE;
    goto done;
  }
  struct endpoint_grant grant = {client, PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL};
  struct endpoint_packet response = {0};
  status = endpoint_request(publication, NULL, 0, &grant, 1,
      now + PUBLICATION_TIMEOUT_NS, &response);
  enum call_status closed = close_handle(&client);
  enum call_status grants_closed = close_grants(&response);
  if (status == CALL_OK && (closed != CALL_OK || grants_closed != CALL_OK)) {
    status = closed != CALL_OK ? closed : grants_closed;
  }
  if (status == CALL_OK && (response.result != CALL_OK || response.size != 0 ||
      response.grant_count != 0)) {
    status = response.result != CALL_OK && response.result < CALL_STATUS_COUNT ?
        response.result : CALL_BAD_REQUEST;
  }
  if (status != CALL_OK) {
    goto done;
  }
  provider.published = true;
  closed = close_handle(&publication);
  enum call_status clock_closed = close_handle(&clock);
  status = closed != CALL_OK ? closed : clock_closed;
  if (status == CALL_OK) {
    status = serve(&provider);
  }

done:
  close_handle(&client);
  close_handle(&endpoint.caller);
  enum call_status receiver_closed = close_handle(&endpoint.receiver);
  if (status == CALL_OK) {
    status = receiver_closed;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "textfs: provider failed (status %u)\n", status);
  }
  return status == CALL_OK ? 0 : 1;
}
