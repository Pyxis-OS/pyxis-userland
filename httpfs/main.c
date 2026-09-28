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
#include "../common/dns.h"
#include "../libhttp/http.h"
#include "../common/provider_setup.h"
#include "trust.h"

#define SERVICE_ID UINT64_C(1)
#define FILE_FIRST_ID UINT64_C(2)
#define FILE_SLOTS (ENDPOINT_EXPORTS_MAX - 1)

_Static_assert(HTTP_MEDIA_TYPE_MAX <= PROVIDER_MEDIA_TYPE_MAX_BYTES,
    "HTTP media type fits OPEN metadata");

struct file_export {
  struct http_body body;
  /* A slot remains reserved until its export's RETIRE is acknowledged. */
  bool live;
};

struct provider {
  handle_t service;
  handle_t receiver;
  struct http_client client;
  struct http_storage storage;
  bool published;
  struct file_export files[FILE_SLOTS];
};

static enum call_status close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return CALL_OK;
  }
  enum call_status status = handle_close(*handle) == 0 ? CALL_OK : CALL_BAD_HANDLE;
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

static enum call_status open_error(struct endpoint_packet *packet,
    enum call_status status, unsigned http_status)
{
  struct provider_open_reply response = {.provider_status = http_status};
  return reply(packet, status, &response, sizeof(response), NULL);
}

static enum call_status open_file(struct provider *provider,
    struct endpoint_packet *packet)
{
  if (packet->operation != PROVIDER_OPEN) {
    return open_error(packet, CALL_BAD_OPERATION, 0);
  }
  if (packet->size < sizeof(struct provider_open_request)) {
    return open_error(packet, CALL_BAD_REQUEST, 0);
  }
  struct provider_open_request request;
  memcpy(&request, packet->data, sizeof(request));
  if (request.rights == 0 || (request.rights & ~FILE_RIGHTS) ||
      request.uri_size == 0 || request.uri_size > PROVIDER_URI_MAX_BYTES ||
      packet->size - sizeof(request) != request.uri_size) {
    return open_error(packet, CALL_BAD_REQUEST, 0);
  }
  if (((request.rights & FILE_RIGHT_READ) &&
      !(packet->rights & PROVIDER_RIGHT_OPEN_READ)) ||
      ((request.rights & FILE_RIGHT_WRITE) &&
      !(packet->rights & PROVIDER_RIGHT_OPEN_WRITE))) {
    return open_error(packet, CALL_DENIED, 0);
  }
  if (request.rights & FILE_RIGHT_WRITE) {
    return open_error(packet, CALL_DENIED, 0);
  }
  if (request.uri_size > HTTP_URI_MAX) {
    return open_error(packet, CALL_FILE_TOO_LARGE, 0);
  }
  const uint8_t *uri_bytes = packet->data + sizeof(request);
  if (memchr(uri_bytes, 0, request.uri_size)) {
    return open_error(packet, CALL_BAD_REQUEST, 0);
  }
  size_t slot = 0;
  while (slot < FILE_SLOTS && provider->files[slot].live) {
    ++slot;
  }
  if (slot == FILE_SLOTS) {
    return open_error(packet, CALL_QUEUE_FULL, 0);
  }
  char uri[HTTP_URI_MAX + 1];
  memcpy(uri, uri_bytes, request.uri_size);
  uri[request.uri_size] = 0;
  struct http_result result;
  http_fetch(&provider->client, &provider->storage, uri, packet->deadline_ns, &result);
  enum call_status status = http_result_status(&result);
  if (result.error == HTTP_TLS_ERROR) {
    fprintf(stderr, "httpfs: TLS fetch failed (%s, native %u, library %d, verify 0x%x)\n",
        tls_error_name(result.tls_failure.error), result.tls_failure.native_status,
        result.tls_failure.library_error, result.tls_failure.verify_flags);
  }
  if (result.tls_cleanup.error != TLS_OK) {
    fprintf(stderr, "httpfs: TLS close failed (%s, native %u, library %d)\n",
        tls_error_name(result.tls_cleanup.error), result.tls_cleanup.native_status,
        result.tls_cleanup.library_error);
  }
  if (status != CALL_OK) {
    return open_error(packet, status, result.status);
  }
  handle_t client = HANDLE_INVALID;
  status = endpoint_export(provider->service, provider->receiver,
      FILE_FIRST_ID + slot, PROTOCOL_FILE, FILE_RIGHT_READ,
      HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    http_body_release(&result.body);
    if (status == CALL_OUTCOME_UNKNOWN) {
      endpoint_finish(packet->receipt);
      return status;
    }
    return open_error(packet, status, result.status);
  }
  provider->files[slot] = (struct file_export){.body = result.body, .live = true};
  struct provider_open_reply header = {
    .protocol = PROTOCOL_FILE,
    .representation = PROVIDER_REPRESENTATION_BYTES,
    .media_type_size = strlen(result.media_type),
    .provider_status = result.status,
  };
  uint8_t response[sizeof(header) + HTTP_MEDIA_TYPE_MAX];
  memcpy(response, &header, sizeof(header));
  memcpy(response + sizeof(header), result.media_type, header.media_type_size);
  struct endpoint_grant grant = {client, FILE_RIGHT_READ, HANDLE_TRANSPORT_CALL};
  status = endpoint_reply(packet->receipt, CALL_OK, response,
      sizeof(header) + header.media_type_size, &grant, 1);
  /* Canceled transfers retain their body until RETIRE acknowledges the export. */
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
    struct file_size_reply response = {file->body.size};
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
  if (request.offset < file->body.size) {
    read = file->body.size - request.offset;
    if (read > request.capacity) {
      read = request.capacity;
    }
  }
  uint8_t response[FILE_PAYLOAD_MAX];
  struct file_read_reply header = {read};
  memcpy(response, &header, sizeof(header));
  if (read != 0) {
    memcpy(response + sizeof(header), file->body.data + request.offset, read);
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
        http_body_release(&provider->files[packet.object_id - FILE_FIRST_ID].body);
      } else {
        return CALL_BAD_REQUEST;
      }
      status = endpoint_retire_ack(provider->receiver, packet.object_id);
      if (status != CALL_OK) {
        return status;
      }
      if (packet.object_id != SERVICE_ID) {
        provider->files[packet.object_id - FILE_FIRST_ID].live = false;
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
      if (packet.object_id == SERVICE_ID && packet.protocol == PROTOCOL_PROVIDER) {
        status = open_error(&packet, CALL_BAD_REQUEST, 0);
      } else {
        status = reply(&packet, CALL_BAD_REQUEST, NULL, 0, NULL);
      }
    } else if (packet.object_id == SERVICE_ID && packet.protocol == PROTOCOL_PROVIDER &&
        provider->published) {
      status = open_file(provider, &packet);
    } else if (packet.object_id >= FILE_FIRST_ID &&
        packet.object_id - FILE_FIRST_ID < FILE_SLOTS && packet.protocol == PROTOCOL_FILE &&
        provider->files[packet.object_id - FILE_FIRST_ID].live) {
      status = serve_file(&provider->files[packet.object_id - FILE_FIRST_ID], &packet);
    } else {
      if (packet.object_id == SERVICE_ID && packet.protocol == PROTOCOL_PROVIDER) {
        status = open_error(&packet, CALL_BAD_REQUEST, 0);
      } else {
        status = reply(&packet, CALL_BAD_REQUEST, NULL, 0, NULL);
      }
    }
    if (status != CALL_OK) {
      return status;
    }
  }
}

int main(int argc, char **argv)
{
  handle_t service = startup_resource("service");
  handle_t publication = startup_resource("publication");
  handle_t clock = startup_resource("clock");
  struct http_authority authority = {
    .tcp = startup_resource("tcp"), .udp = startup_resource("udp"),
    .random = startup_resource("random"), .clock = clock,
  };
  struct endpoint_create_reply endpoint = {0};
  handle_t client = HANDLE_INVALID;
  struct provider provider = {
    .service = service, .client = {.authority = authority, .scheme = HTTP_SCHEME_HTTP},
  };
  enum call_status status = CALL_OK;
  enum call_status cleanup_status = CALL_OK;
  bool publication_attempted = false;
  const char *custom_bundle = NULL;
  if (argc == 2 && !strcmp(argv[1], "--https")) {
    provider.client.scheme = HTTP_SCHEME_HTTPS;
  } else if (argc == 4 && !strcmp(argv[1], "--https") &&
      !strcmp(argv[2], "--ca-bundle") && argv[3][0]) {
    provider.client.scheme = HTTP_SCHEME_HTTPS;
    custom_bundle = argv[3];
  } else if (argc != 1) {
    fputs("usage: httpfs [--https [--ca-bundle URI]]\n", stderr);
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (service == HANDLE_INVALID || publication == HANDLE_INVALID || clock == HANDLE_INVALID) {
    fputs("httpfs: missing provider authority\n", stderr);
    status = CALL_UNAVAILABLE;
    goto done;
  }
  if (!dns_select_server(NULL, &provider.client.authority.dns_server)) {
    fputs("httpfs: invalid DNS_SERVER\n", stderr);
    status = CALL_BAD_REQUEST;
    goto done;
  }
  if (provider.client.scheme == HTTP_SCHEME_HTTPS) {
    struct tls_authority tls_authority = {.random = authority.random, .clock = clock};
    struct tls_result result;
    if (!tls_runtime_create(&tls_authority, &provider.client.tls, &result)) {
      struct http_result failure = {.error = HTTP_TLS_ERROR, .tls_failure = result};
      status = http_result_status(&failure);
    } else {
      status = httpfs_trust_load(provider.client.tls, HTTPFS_PUBLIC_BUNDLE,
          &result, &cleanup_status);
      if (status == CALL_OK && custom_bundle) {
        status = httpfs_trust_load(provider.client.tls, custom_bundle,
            &result, &cleanup_status);
      }
      if (status == CALL_OK && !tls_runtime_ready(provider.client.tls, &result)) {
        struct http_result failure = {.error = HTTP_TLS_ERROR, .tls_failure = result};
        status = http_result_status(&failure);
      }
    }
    if (status != CALL_OK) {
      fprintf(stderr, "httpfs: TLS setup failed (%s, native %u, library %d, verify 0x%x)\n",
          tls_error_name(result.error), result.native_status,
          result.library_error, result.verify_flags);
      goto done;
    }
  }
  status = endpoint_create(service, &endpoint);
  if (status != CALL_OK) {
    goto done;
  }
  provider.receiver = endpoint.receiver;
  status = close_handle(&endpoint.caller);
  if (status != CALL_OK) {
    cleanup_status = status;
    goto done;
  }
  status = endpoint_export(service, endpoint.receiver, SERVICE_ID, PROTOCOL_PROVIDER,
      PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL, &client);
  if (status != CALL_OK) {
    goto done;
  }
  struct endpoint_grant grant = {client, PROVIDER_RIGHT_OPEN_READ, HANDLE_TRANSPORT_CALL};
  status = provider_setup_report(publication, clock, CALL_OK, CALL_OK, &grant, &publication_attempted);
  enum call_status closed = close_handle(&client);
  if (closed != CALL_OK) {
    cleanup_status = closed;
  }
  if (status == CALL_OK) {
    status = closed;
  }
  if (status != CALL_OK) {
    goto done;
  }
  provider.published = true;
  status = close_handle(&publication);
  if (status == CALL_OK) {
    status = serve(&provider);
  }

done:
  enum call_status closed_client = close_handle(&client);
  enum call_status caller_closed = close_handle(&endpoint.caller);
  enum call_status receiver_closed = close_handle(&endpoint.receiver);
  if (closed_client != CALL_OK || caller_closed != CALL_OK || receiver_closed != CALL_OK) {
    cleanup_status = CALL_BAD_HANDLE;
  }
  for (size_t i = 0; i < FILE_SLOTS; ++i) {
    http_body_release(&provider.files[i].body);
  }
  tls_runtime_free(provider.client.tls);
  if (status == CALL_OK) {
    status = cleanup_status;
  }
  if (!publication_attempted && publication != HANDLE_INVALID) {
    enum call_status reported = provider_setup_report(publication, clock,
        status, cleanup_status, NULL, NULL);
    if (reported != CALL_OK) {
      fprintf(stderr, "httpfs: setup report failed (status %u)\n", reported);
    }
  }
  enum call_status publication_closed = close_handle(&publication);
  if (status == CALL_OK) {
    status = publication_closed;
  }
  if (status != CALL_OK) {
    fprintf(stderr, "httpfs: provider failed (status %u)\n", status);
  }
  return status == CALL_OK ? 0 : 1;
}
