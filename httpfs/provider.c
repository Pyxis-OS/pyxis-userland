#include "provider.h"
#include <abi/completion.h>
#include <abi/directory.h>
#include <abi/http_transaction.h>
#include <abi/transaction.h>
#include <clock.h>
#include <completion.h>
#include <directory.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <forwarder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wait.h>

#define EXPORT_SLOTS (ENDPOINT_EXPORTS_MAX - HTTPFS_ROOT_COUNT)
#define RESPONSE_BYTES 8192u
#define REQUEST_FIELDS_MAX 256u

_Static_assert((unsigned)HTTP_RESPONSE_PENDING == (unsigned)HTTP_FRAMING_PENDING &&
    (unsigned)HTTP_RESPONSE_COMPLETE == (unsigned)HTTP_FRAMING_COMPLETE &&
    (unsigned)HTTP_RESPONSE_FAILED == (unsigned)HTTP_FRAMING_FAILED, "HTTP framing ABI encoding");

enum facet { FACET_RESERVED, FACET_CONTROL, FACET_BODY, FACET_OBSERVATION,
  FACET_SNAPSHOT };
enum staging_operation { STAGING_NONE, STAGING_CREATE, STAGING_UNLINK,
  STAGING_WRITE, STAGING_RESIZE, SNAPSHOT_WRITE, SNAPSHOT_READ };

struct transaction;
struct exported {
  handle_t controller;
  struct transaction *owner;
  enum facet facet;
  bool live;
};

struct transaction_work {
  struct endpoint_packet preparation, commit, mutation, read, abort;
  char uri[HTTP_URI_MAX + 1], next_uri[HTTP_URI_MAX + 1], name[64];
  char visited[HTTP_REDIRECT_MAX + 1][HTTP_URL_KEY_BYTES];
  char fields[ENDPOINT_DATA_MAX];
  struct http_request_header headers_fields[REQUEST_FIELDS_MAX];
  size_t field_count;
};

struct transaction {
  struct transaction_work *work;
  struct http_response response;
  struct http_stream *engine;
  struct http_wait wait;
  handle_t snapshot;
  uint64_t snapshot_size, snapshot_charge, body_charge;
  unsigned char *error_body;
  size_t error_size, redirects;
  bool generic, removing, creating_snapshot, redirecting, mutation_queued;
  bool attempted, sent, origin_crossed, response_charged;
  uint64_t uploaded, read_offset, read_capacity;
  struct provider_http_budget charged;
  char detail[RESOURCE_DETAIL_MAX_BYTES + 1];
  struct http_prepare_context prepare_context;
  char uri[HTTP_URI_MAX + 1];
  enum http_transaction_phase phase;
  enum call_status failure, cleanup, mutation_failure, read_failure;
  bool headers, handed_off, aborting, linked, unlink_attempted;
  bool terminal_description_published;
  uint64_t method, flags, deadline, body_length, consumed;
  uint64_t mutation_offset, mutation_size, pending;
  enum staging_operation staging_operation;
  handle_t backing, body_source;
  size_t slots[5], slot_count;
  size_t queued;
  unsigned char *queue;
};

struct loop {
  handle_t service, receiver, staging;
  handle_t roots[HTTPFS_ROOT_COUNT];
  uint64_t body_cap, storage_cap, reserved;
  struct transaction *storage_owner;
  struct http_client client;
  struct exported exports[EXPORT_SLOTS];
  struct transaction *active;
  uint64_t next_id, next_name;
  bool custom_trust;
  enum call_status fatal;
};

static void erase(void *data, size_t size)
{
  volatile unsigned char *bytes = data;
  while (size--) {
    *bytes++ = 0;
  }
}

static void clear_packet(struct endpoint_packet *packet)
{
  erase(packet, sizeof(*packet));
}

static enum call_status reply(struct loop *loop, struct endpoint_packet *packet,
    enum call_status status, const void *data, size_t size,
    const struct endpoint_grant *grants, size_t count)
{
  enum call_status sent = endpoint_reply(packet->receipt, status, data, size, grants, count);
  if (sent != CALL_OK && endpoint_finish(packet->receipt) != CALL_OK) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  clear_packet(packet);
  return sent;
}

static void finish(struct loop *loop, struct endpoint_packet *packet)
{
  if (packet->receipt != HANDLE_INVALID && endpoint_finish(packet->receipt) != CALL_OK) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  clear_packet(packet);
}

static uint64_t object_id(size_t slot)
{
  return slot + HTTPFS_TRANSACTION_OBJECT_BASE;
}

static uint64_t facet_protocol(enum facet facet)
{
  return facet == FACET_BODY || facet == FACET_SNAPSHOT ? PROTOCOL_FILE :
      facet == FACET_CONTROL ? PROTOCOL_TRANSACTION : PROTOCOL_HTTP_TRANSACTION;
}

static uint64_t facet_rights(enum facet facet)
{
  switch (facet) {
  case FACET_BODY: return FILE_RIGHT_WRITE;
  case FACET_SNAPSHOT: return FILE_RIGHT_READ;
  case FACET_CONTROL: return TRANSACTION_RIGHTS;
  case FACET_OBSERVATION: return HTTP_TRANSACTION_RIGHT_STATUS;
  default: return 0;
  }
}

static enum call_status export(struct loop *loop, struct transaction *transaction,
    size_t position, enum facet facet, handle_t *handle)
{
  size_t slot = transaction->slots[position];
  enum call_status status = endpoint_export(loop->service, loop->receiver, object_id(slot),
      facet_protocol(facet), facet_rights(facet), HANDLE_TRANSPORT_CALL, handle,
      &loop->exports[slot].controller);
  if (status == CALL_OK) {
    loop->exports[slot].facet = facet;
    loop->exports[slot].live = true;
  } else if (status == CALL_OUTCOME_UNKNOWN) {
    /* Unknown export admission cannot safely reuse its ID or storage. */
    loop->fatal = status;
  }
  return status;
}

static bool has_facet(const struct loop *loop, const struct transaction *transaction,
    enum facet facet)
{
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    const struct exported *entry = &loop->exports[transaction->slots[i]];
    if (entry->owner == transaction && entry->live && entry->facet == facet) {
      return true;
    }
  }
  return false;
}

static enum call_status response_status(const struct http_response *response)
{
  if (response->error == HTTP_SOURCE_ERROR) {
    return response->source_status == CALL_OK ? CALL_OUTCOME_UNKNOWN : response->source_status;
  }
  struct http_result result = {.error = response->error,
    .network_status = response->network_status, .tls_failure = response->tls_failure,
    .status = response->status, .dns_rcode = response->dns_rcode};
  return http_result_status(&result);
}

static struct resource_description transaction_description(const struct transaction *transaction)
{
  struct resource_description description = {0};
  switch (transaction->failure) {
  case CALL_OK: description.outcome = RESOURCE_OK; break;
  case CALL_NOT_FOUND: description.outcome = RESOURCE_NOT_FOUND; break;
  case CALL_DENIED: description.outcome = RESOURCE_DENIED; break;
  case CALL_FILE_TOO_LARGE: description.outcome = RESOURCE_TOO_LARGE; break;
  case CALL_ABANDONED: description.outcome = RESOURCE_ABORTED; break;
  case CALL_OUTCOME_UNKNOWN: description.outcome = RESOURCE_UNCERTAIN; break;
  default: description.outcome = RESOURCE_IO_ERROR; break;
  }
  if (transaction->phase == HTTP_TRANSACTION_COMPLETE) {
    description.flags |= RESOURCE_COMPLETE;
  }
  if (transaction->headers && transaction->response.length_present) {
    description.flags |= RESOURCE_LENGTH_KNOWN;
    description.length = transaction->response.length;
  }
  if (transaction->headers) {
    size_t length = strnlen(transaction->response.media_type,
        sizeof(transaction->response.media_type));
    bool ascii = true;
    for (size_t i = 0; i < length; ++i) {
      ascii &= (unsigned char)transaction->response.media_type[i] < 0x80;
    }
    if (length <= RESOURCE_MEDIA_TYPE_MAX_BYTES && ascii) {
      memcpy(description.media_type, transaction->response.media_type, length);
      description.media_type_size = length;
    }
  }
  if (transaction->phase == HTTP_TRANSACTION_COMPLETE && transaction->failure == CALL_OK) {
    description.flags |= RESOURCE_REREADABLE | RESOURCE_LENGTH_KNOWN;
    description.length = transaction->snapshot_size;
  }
  if (description.outcome != RESOURCE_OK) {
    if (transaction->error_body || (transaction->headers && transaction->response.status >= 400)) {
      description.flags |= RESOURCE_ERROR_BODY;
    }
    const char *detail = transaction->detail[0] ? transaction->detail :
        description.outcome == RESOURCE_ABORTED ? "Transaction abandoned" :
        description.outcome == RESOURCE_UNCERTAIN ? "Submission outcome unknown" :
        "Transaction failed";
    int length = transaction->response.status ?
        snprintf(description.detail, sizeof(description.detail), "%s (HTTP %u)", detail, transaction->response.status) :
        snprintf(description.detail, sizeof(description.detail), "%s", detail);
    description.detail_size = length > 0 && (size_t)length < sizeof(description.detail) ? (size_t)length : 0;
  }
  return description;
}

static enum call_status publish_description(struct loop *loop,
    struct transaction *transaction)
{
  struct resource_description description = transaction_description(transaction);
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    size_t slot = transaction->slots[i];
    struct exported *entry = &loop->exports[slot];
    if (!entry->live || (entry->facet != FACET_CONTROL && entry->facet != FACET_SNAPSHOT)) {
      continue;
    }
    enum call_status status = endpoint_describe_publish(loop->receiver, object_id(slot),
        &description, description.outcome == RESOURCE_OK ? NULL : transaction->error_body,
        description.outcome == RESOURCE_OK ? 0 : transaction->error_size);
    /* Last-client close can precede its queued RETIRE notice. There is no
     * reader to publish to, but keep the slot live until retirement is acked. */
    if (status != CALL_OK && status != CALL_ENDPOINT_CLOSED) {
      return status;
    }
  }
  return CALL_OK;
}

static void release_body_source(struct loop *loop, struct transaction *transaction)
{
  if (transaction->body_source && handle_close(transaction->body_source)) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  transaction->body_source = HANDLE_INVALID;
}

static void cut_exports(struct loop *loop, struct transaction *transaction)
{
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    struct exported *entry = &loop->exports[transaction->slots[i]];
    if (entry->owner == transaction && entry->live) {
      enum call_status status = forwarder_cut(entry->controller);
      if (status != CALL_OK && status != CALL_NOT_FOUND) {
        loop->fatal = status;
      }
    }
  }
}


static bool storage_reserve(struct loop *loop, uint64_t *charge, uint64_t length)
{
  if (!loop->storage_cap || length <= *charge) {
    return true;
  }
  uint64_t growth = length - *charge;
  if (loop->reserved > loop->storage_cap || growth > loop->storage_cap - loop->reserved) {
    return false;
  }
  loop->reserved += growth;
  *charge = length;
  return true;
}

static void storage_release(struct loop *loop, uint64_t *charge, uint64_t length)
{
  if (loop->storage_cap && length < *charge) {
    loop->reserved -= *charge - length;
    *charge = length;
  }
}

static void close_backing(struct loop *loop, handle_t *handle, uint64_t *charge)
{
  if (*handle && handle_close(*handle)) {
    loop->fatal = CALL_BAD_HANDLE;
    return;
  }
  *handle = HANDLE_INVALID;
  storage_release(loop, charge, 0);
}
static void destroy_if_retired(struct loop *loop, struct transaction *transaction)
{
  if (transaction->engine || transaction->pending || (transaction->work &&
      (transaction->work->preparation.receipt || transaction->work->commit.receipt ||
      transaction->work->mutation.receipt || transaction->work->read.receipt ||
      transaction->work->abort.receipt))) {
    return;
  }
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    if (loop->exports[transaction->slots[i]].live) {
      return;
    }
  }
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    loop->exports[transaction->slots[i]] = (struct exported){0};
  }
  if (loop->active == transaction) {
    loop->active = NULL;
  }
  close_backing(loop, &transaction->backing, &transaction->body_charge);
  close_backing(loop, &transaction->snapshot, &transaction->snapshot_charge);
  if (transaction->error_body) {
    erase(transaction->error_body, transaction->error_size);
    free(transaction->error_body);
  }
  if (transaction->work) {
    erase(transaction->work, sizeof(*transaction->work));
    free(transaction->work);
  }
  if (transaction->queue) {
    erase(transaction->queue, RESPONSE_BYTES);
    free(transaction->queue);
  }
  erase(transaction, sizeof(*transaction));
  free(transaction);
}

static void abort_transaction(struct loop *loop, struct transaction *transaction,
    enum call_status failure)
{
  if (transaction->phase == HTTP_TRANSACTION_COMPLETE) {
    return;
  }
  if (!transaction->aborting) {
    release_body_source(loop, transaction);
    transaction->aborting = true;
    transaction->failure = failure;
    transaction->phase = failure == CALL_ABANDONED ? HTTP_TRANSACTION_ABORTED : HTTP_TRANSACTION_FAILED;
  }
  if (transaction->pending) {
    enum call_status status = completion_cancel(loop->receiver, transaction->pending);
    if (status != CALL_OK && status != CALL_NOT_FOUND) {
      transaction->cleanup = status;
    }
  }
  if (transaction->engine) {
    enum call_status status = http_stream_abort(transaction->engine);
    if (status != CALL_OK) {
      transaction->cleanup = status;
    }
  }
}

static enum call_status reserve(struct loop *loop, size_t count,
    struct transaction **result)
{
  if (loop->active) {
    return CALL_BUSY;
  }
  size_t slots[5], found = 0;
  for (size_t i = 0; i < EXPORT_SLOTS && found < count; ++i) {
    if (!loop->exports[i].owner) {
      slots[found++] = i;
    }
  }
  if (found != count) {
    return CALL_LIMIT;
  }
  struct transaction *transaction = calloc(1, sizeof(*transaction));
  if (!transaction) {
    return CALL_NO_MEMORY;
  }
  transaction->work = calloc(1, sizeof(*transaction->work));
  transaction->queue = malloc(RESPONSE_BYTES);
  if (!transaction->work || !transaction->queue) {
    free(transaction->queue);
    free(transaction->work);
    free(transaction);
    return CALL_NO_MEMORY;
  }
  transaction->phase = HTTP_TRANSACTION_PREPARING;
  transaction->slot_count = count;
  for (size_t i = 0; i < count; ++i) {
    transaction->slots[i] = slots[i];
    loop->exports[slots[i]].owner = transaction;
  }
  loop->active = transaction;
  *result = transaction;
  return CALL_OK;
}

static uint64_t native_deadline(struct loop *loop, struct transaction *transaction,
    bool cleanup)
{
  uint64_t now;
  enum call_status status = clock_now(loop->client.authority.clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
    transaction->cleanup = status == CALL_OK ? CALL_LIMIT : status;
    return 0;
  }
  uint64_t maximum = now + WAIT_MAX_WAIT_NS;
  return !cleanup && transaction->deadline < maximum ? transaction->deadline : maximum;
}

static enum call_status submit(struct loop *loop, struct transaction *transaction,
    handle_t target, uint64_t protocol, uint64_t operation,
    const void *bytes, size_t size, enum staging_operation kind, bool cleanup)
{
  if (loop->storage_owner || (loop->active && loop->active->engine &&
      http_stream_pending(loop->active->engine))) {
    return CALL_BUSY;
  }
  if (loop->next_id == UINT64_MAX) {
    return CALL_LIMIT;
  }
  uint64_t deadline = native_deadline(loop, transaction, cleanup);
  if (!deadline) {
    return transaction->cleanup;
  }
  uint64_t id = loop->next_id;
  enum call_status status = completion_submit(target, loop->receiver, id,
      protocol, operation, deadline, bytes, size);
  if (status == CALL_OK) {
    ++loop->next_id;
    transaction->pending = id;
    loop->storage_owner = transaction;
    transaction->staging_operation = kind;
  }
  return status;
}

static enum call_status unlink_backing(struct loop *loop, struct transaction *transaction)
{
  size_t length = strlen(transaction->work->name);
  unsigned char bytes[sizeof(struct completion_remove_request) + sizeof(transaction->work->name)];
  struct completion_remove_request request = {.name_length = length, .kind = DIRECTORY_KIND_FILE};
  memcpy(bytes, &request, sizeof(request));
  memcpy(bytes + sizeof(request), transaction->work->name, length);
  enum call_status status = submit(loop, transaction, loop->staging, PROTOCOL_DIRECTORY, DIRECTORY_REMOVE,
      bytes, sizeof(request) + length, STAGING_UNLINK, transaction->aborting);
  transaction->unlink_attempted = true;
  return status;
}

enum call_status httpfs_staging_validate(handle_t staging, uint64_t *filesystem_type)
{
  if (!filesystem_type) {
    return CALL_BAD_REQUEST;
  }
  *filesystem_type = 0;
  struct handle_info handle;
  enum call_status status = handle_query(staging, &handle);
  uint64_t required = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES | DIRECTORY_RIGHT_FILESYSTEM_INFO;
  if (status != CALL_OK) {
    return status;
  }
  if (handle.kind != HANDLE_KIND_NATIVE || handle.protocol != PROTOCOL_DIRECTORY || handle.transport) {
    return CALL_BAD_OPERATION;
  }
  if ((handle.rights & required) != required) {
    return CALL_DENIED;
  }
  struct directory_filesystem_info info;
  status = directory_filesystem_info(staging, &info);
  if (status != CALL_OK) {
    return status;
  }
  if ((info.type != FILESYSTEM_TYPE_NPFS && info.type != FILESYSTEM_TYPE_RAM) ||
      (info.flags & FILESYSTEM_FLAG_READ_ONLY)) {
    return CALL_DENIED;
  }
  *filesystem_type = info.type;
  return CALL_OK;
}


static enum call_status create_backing(struct loop *loop, struct transaction *transaction,
    bool snapshot)
{
  if (!loop->staging) {
    return CALL_UNAVAILABLE;
  }
  if (loop->next_name == UINT64_MAX) {
    return CALL_LIMIT;
  }
  transaction->creating_snapshot = snapshot;
  transaction->linked = false;
  transaction->unlink_attempted = false;
  snprintf(transaction->work->name, sizeof(transaction->work->name), "http-private-%llu",
      (unsigned long long)++loop->next_name);
  size_t length = strlen(transaction->work->name);
  unsigned char bytes[sizeof(struct completion_create_request) + sizeof(transaction->work->name)];
  struct completion_create_request creation = {.name_length = length,
    .kind = DIRECTORY_KIND_FILE, .rights = FILE_RIGHTS};
  memcpy(bytes, &creation, sizeof(creation));
  memcpy(bytes + sizeof(creation), transaction->work->name, length);
  return submit(loop, transaction, loop->staging, PROTOCOL_DIRECTORY, DIRECTORY_CREATE,
      bytes, sizeof(creation) + length, STAGING_CREATE, false);
}

static enum call_status default_deadline(struct loop *loop, uint64_t requested,
    uint64_t *deadline)
{
  uint64_t now;
  enum call_status status = clock_now(loop->client.authority.clock, &now);
  if (status != CALL_OK) {
    return status;
  }
  if (requested) {
    if (requested <= now) {
      return CALL_TIMED_OUT;
    }
    *deadline = requested;
  } else {
    if (now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
      return CALL_LIMIT;
    }
    *deadline = now + WAIT_MAX_WAIT_NS;
  }
  return CALL_OK;
}

static enum call_status chain_initialize(struct transaction *transaction)
{
  struct http_uri uri;
  enum call_status status = http_url_parse(transaction->work->uri, &uri);
  if (status != CALL_OK) {
    return status;
  }
  struct http_prepare_context *context = &transaction->prepare_context;
  context->origin.scheme = uri.scheme;
  context->origin.port = uri.port;
  memcpy(context->origin.host, uri.host, strlen(uri.host) + 1);
  context->remaining = (struct http_framing_budget){
    HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX};
  return http_url_key(transaction->work->uri, transaction->work->visited[0]);
}
static bool parse_fields(struct transaction *transaction, size_t size)
{
  size_t offset = 0;
  while (offset < size) {
    if (transaction->work->field_count == REQUEST_FIELDS_MAX) {
      return false;
    }
    char *line = transaction->work->fields + offset;
    char *end = memchr(line, '\r', size - offset);
    if (!end || end + 1 == transaction->work->fields + size || end[1] != '\n') {
      return false;
    }
    char *colon = memchr(line, ':', end - line);
    if (!colon || colon == line) {
      return false;
    }
    char *value = colon + 1;
    while (value < end && (*value == ' ' || *value == '\t')) {
      ++value;
    }
    struct http_request_header *header = &transaction->work->headers_fields[transaction->work->field_count++];
    *header = (struct http_request_header){line, value, colon - line, end - value};
    if ((header->name_length == 13 && !strncasecmp(line, "authorization", 13)) ||
        (header->name_length == 6 && !strncasecmp(line, "cookie", 6)) ||
        (header->name_length == 19 && !strncasecmp(line, "proxy-authorization", 19))) {
      transaction->flags |= HTTP_PREPARE_CREDENTIALS;
    }
    offset = end - transaction->work->fields + 2;
  }
  if (transaction->flags & HTTP_PREPARE_JSON) {
    bool content_type = false, accept = false;
    for (size_t i = 0; i < transaction->work->field_count; ++i) {
      struct http_request_header *header = &transaction->work->headers_fields[i];
      content_type |= header->name_length == 12 && !strncasecmp(header->name, "content-type", 12);
      accept |= header->name_length == 6 && !strncasecmp(header->name, "accept", 6);
    }
    if (!accept) {
      if (transaction->work->field_count == REQUEST_FIELDS_MAX) {
        return false;
      }
      transaction->work->headers_fields[transaction->work->field_count++] = (struct http_request_header){
        "Accept", "application/json", 6, 16};
    }
    if (!content_type) {
      if (transaction->work->field_count == REQUEST_FIELDS_MAX) {
        return false;
      }
      transaction->work->headers_fields[transaction->work->field_count++] = (struct http_request_header){
        "Content-Type", "application/json", 12, 16};
    }
  }
  return true;
}

static void preparation_ready(struct loop *loop, struct transaction *transaction)
{
  if (transaction->aborting || !transaction->work->preparation.receipt) {
    return;
  }
  handle_t handles[3] = {0};
  bool post = transaction->method == HTTP_PREPARE_POST;
  enum facet facets[] = {FACET_CONTROL, post ? FACET_BODY : FACET_OBSERVATION, FACET_OBSERVATION};
  size_t count = post ? 3 : 2;
  if (transaction->generic) {
    count = 2;
  }
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < count && status == CALL_OK; ++i) {
    status = export(loop, transaction, i, facets[i], &handles[i]);
  }
  if (status == CALL_OK) {
    status = publish_description(loop, transaction);
  }
  if (status == CALL_OK) {
    if (transaction->generic) {
      struct provider_open_reply response = {.protocol = PROTOCOL_TRANSACTION,
        .description = transaction_description(transaction)};
      struct endpoint_grant grant = {handles[0], TRANSACTION_RIGHTS, HANDLE_TRANSPORT_CALL};
      status = reply(loop, &transaction->work->preparation, CALL_OK, &response,
          sizeof(response), &grant, 1);
    } else {
      struct endpoint_grant grants[3];
      for (size_t i = 0; i < count; ++i) {
        grants[i] = (struct endpoint_grant){handles[i], facet_rights(facets[i]), HANDLE_TRANSPORT_CALL};
      }
      struct http_prepare_reply response = {.method = transaction->method,
        .trust_flags = loop->custom_trust ? HTTP_TRUST_ORIGIN_ONLY : 0};
      status = reply(loop, &transaction->work->preparation, CALL_OK, &response, sizeof(response), grants, count);
    }
  } else {
    reply(loop, &transaction->work->preparation, status, NULL, 0, NULL, 0);
  }
  if (status == CALL_OK && post) {
    transaction->body_source = handles[1];
    handles[1] = HANDLE_INVALID;
  }
  for (size_t i = 0; i < count; ++i) {
    if (handles[i] && handle_close(handles[i])) {
      loop->fatal = CALL_BAD_HANDLE;
    }
  }
  if (status != CALL_OK) {
    abort_transaction(loop, transaction, status);
    cut_exports(loop, transaction);
  } else {
    transaction->phase = HTTP_TRANSACTION_STAGING;
  }
}

static enum call_status preparation_context(struct loop *loop,
    const struct http_prepare_context *context, const char *text)
{
  if ((context->flags & ~HTTP_PREPARE_CONTEXT_FLAGS) ||
      context->origin.scheme > PROVIDER_HTTPS || !context->origin.port ||
      context->origin.port > UINT16_MAX ||
      context->remaining.header_bytes > HTTP_HEADERS_MAX ||
      context->remaining.fields > HTTP_FIELDS_MAX ||
      context->remaining.informational > HTTP_INFORMATIONAL_MAX) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(context->origin.host, sizeof(context->origin.host));
  if (!length || length == sizeof(context->origin.host)) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = length + 1; i < sizeof(context->origin.host); ++i) {
    if (context->origin.host[i]) {
      return CALL_BAD_REQUEST;
    }
  }
  char origin_text[HTTP_URI_MAX + 1];
  int size = snprintf(origin_text, sizeof(origin_text), "%s://%s:%llu/",
      context->origin.scheme == PROVIDER_HTTPS ? "https" : "http",
      context->origin.host, (unsigned long long)context->origin.port);
  struct http_uri origin, uri;
  if (size < 0 || (size_t)size >= sizeof(origin_text) ||
      http_url_parse(origin_text, &origin) != CALL_OK ||
      origin.scheme != context->origin.scheme || origin.port != context->origin.port ||
      strcmp(origin.host, context->origin.host) || http_url_parse(text, &uri) != CALL_OK) {
    return CALL_BAD_REQUEST;
  }
  bool crossing = context->origin.scheme != uri.scheme ||
      context->origin.port != uri.port || strcmp(context->origin.host, uri.host);
  if (loop->custom_trust && ((context->flags & HTTP_PREPARE_ORIGIN_CROSSED) || crossing)) {
    return CALL_DENIED;
  }
  return CALL_OK;
}

static void prepare(struct loop *loop, size_t root, struct endpoint_packet *packet)
{
  struct http_prepare_request request;
  if (packet->size < sizeof(request)) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  memcpy(&request, packet->data, sizeof(request));
  uint64_t required = request.method == HTTP_PREPARE_GET ? PROVIDER_RIGHT_OPEN_READ : PROVIDER_RIGHT_OPEN_WRITE;
  if (request.method > HTTP_PREPARE_POST || (request.flags & ~HTTP_PREPARE_FLAGS) ||
      !request.uri_size || request.uri_size > HTTP_URI_MAX + 5 ||
      request.uri_size > packet->size - sizeof(request) ||
      request.headers_size != packet->size - sizeof(request) - request.uri_size || !request.deadline_ns) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  if (!(packet->rights & required) || (request.method == HTTP_PREPARE_POST && !loop->staging)) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    return;
  }
  uint64_t now;
  enum call_status status = clock_now(loop->client.authority.clock, &now);
  if (status != CALL_OK || request.deadline_ns <= now) {
    reply(loop, packet, status == CALL_OK ? CALL_TIMED_OUT : status, NULL, 0, NULL, 0);
    return;
  }
  struct transaction *transaction;
  status = reserve(loop, request.method == HTTP_PREPARE_POST ? 4 : 3, &transaction);
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    return;
  }
  transaction->method = request.method;
  transaction->flags = request.flags |
      (root >= HTTPFS_ROOT_JSON_HTTP ? HTTP_PREPARE_JSON : 0);
  transaction->deadline = request.deadline_ns;
  transaction->prepare_context = request.context;
  const char *uri = (const char *)packet->data + sizeof(request);
  size_t uri_size = request.uri_size;
  if (uri_size >= 5 && !memcmp(uri, "json+", 5)) {
    transaction->flags |= HTTP_PREPARE_JSON;
    uri += 5;
    uri_size -= 5;
  }
  if (uri_size > HTTP_URI_MAX || memchr(uri, 0, uri_size) ||
      memchr(packet->data + sizeof(request) + request.uri_size, 0, request.headers_size)) {
    status = CALL_BAD_REQUEST;
  } else {
    memcpy(transaction->work->uri, uri, uri_size);
    memcpy(transaction->uri, uri, uri_size);
    status = preparation_context(loop, &request.context, transaction->work->uri);
    memcpy(transaction->work->fields, packet->data + sizeof(request) + request.uri_size, request.headers_size);
    if (status == CALL_OK && !parse_fields(transaction, request.headers_size)) {
      status = CALL_BAD_REQUEST;
    } else if (status == CALL_OK) {
      struct http_client validation_client = loop->client;
      struct http_uri parsed;
      if (http_url_parse(transaction->work->uri, &parsed) == CALL_OK) {
        bool https = root == HTTPFS_ROOT_HTTPS || root == HTTPFS_ROOT_JSON_HTTPS;
        if (parsed.scheme != (https ? HTTP_SCHEME_HTTPS : HTTP_SCHEME_HTTP)) {
          status = CALL_BAD_OPERATION;
        }
        validation_client.scheme = parsed.scheme;
      }
      struct http_request validation = {.method = request.method,
        .headers = transaction->work->headers_fields, .header_count = transaction->work->field_count};
      if (status == CALL_OK &&
          !http_request_validate(&validation_client, transaction->work->uri, &validation, &transaction->response)) {
        status = response_status(&transaction->response);
      }
    }
  }
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    destroy_if_retired(loop, transaction);
    return;
  }
  if (status == CALL_OK) {
    status = http_url_key(transaction->work->uri, transaction->work->visited[0]);
  }
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    destroy_if_retired(loop, transaction);
    return;
  }
  transaction->response = (struct http_response){0};
  transaction->work->preparation = *packet;
  clear_packet(packet);
  if (request.method == HTTP_PREPARE_GET) {
    preparation_ready(loop, transaction);
    return;
  }
  status = create_backing(loop, transaction, false);
  if (status != CALL_OK) {
    reply(loop, &transaction->work->preparation, status, NULL, 0, NULL, 0);
    abort_transaction(loop, transaction, status);
  }
}


static void open_resource(struct loop *loop, size_t root, struct endpoint_packet *packet)
{
  struct provider_open_request request = {0};
  size_t prefix;
  bool removing = packet->operation == PROVIDER_REMOVE;
  if (removing) {
    struct provider_remove_request removal;
    if (packet->size < sizeof(removal)) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    memcpy(&removal, packet->data, sizeof(removal));
    request.target_size = removal.target_size;
    request.deadline_ns = removal.deadline_ns;
    prefix = sizeof(removal);
  } else {
    if (packet->size < sizeof(request)) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    memcpy(&request, packet->data, sizeof(request));
    prefix = sizeof(request);
  }
  bool write = request.intent & PROVIDER_INTENT_WRITE;
  uint64_t required = removing ? PROVIDER_RIGHT_REMOVE :
      ((request.intent & PROVIDER_INTENT_READ) ? PROVIDER_RIGHT_OPEN_READ : 0) |
      ((write || (request.flags & PROVIDER_OPEN_CREATE)) ? PROVIDER_RIGHT_OPEN_WRITE : 0);
  if ((packet->rights & required) != required) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    return;
  }
  if (request.target_size != packet->size - prefix || !request.target_size ||
      memchr(packet->data + prefix, 0, request.target_size)) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  if (!removing && ((request.flags & (PROVIDER_OPEN_APPEND | PROVIDER_OPEN_EXCLUSIVE)) ||
      (!write && (request.flags & PROVIDER_OPEN_CREATE)))) {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
    return;
  }
  if (!removing && !loop->staging) {
    reply(loop, packet, CALL_UNAVAILABLE, NULL, 0, NULL, 0);
    return;
  }
  struct transaction *transaction;
  enum call_status status = reserve(loop, write ? 3 : 1, &transaction);
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    return;
  }
  transaction->generic = true;
  transaction->removing = removing;
  transaction->method = removing ? HTTP_METHOD_DELETE : write ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  transaction->flags = root >= HTTPFS_ROOT_JSON_HTTP ? HTTP_PREPARE_JSON : 0;
  bool https = root == HTTPFS_ROOT_HTTPS || root == HTTPFS_ROOT_JSON_HTTPS;
  int length = snprintf(transaction->work->uri, sizeof(transaction->work->uri), "%s://%.*s",
      https ? "https" : "http", (int)request.target_size, packet->data + prefix);
  status = length < 0 || (size_t)length >= sizeof(transaction->work->uri) ? CALL_FILE_TOO_LARGE :
      default_deadline(loop, request.deadline_ns, &transaction->deadline);
  if (status == CALL_OK) {
    status = chain_initialize(transaction);
  }
  if (status == CALL_OK && !parse_fields(transaction, 0)) {
    status = CALL_BAD_REQUEST;
  }
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    destroy_if_retired(loop, transaction);
    return;
  }
  memcpy(transaction->uri, transaction->work->uri, sizeof(transaction->uri));
  transaction->work->preparation = *packet;
  clear_packet(packet);
  if (write) {
    status = create_backing(loop, transaction, false);
    if (status != CALL_OK) {
      abort_transaction(loop, transaction, status);
    }
  } else {
    transaction->phase = HTTP_TRANSACTION_OPENING;
  }
}
static void publish_status(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct http_response *response = &transaction->response;
  struct http_transaction_status status = {
    .phase = transaction->phase,
    .submission = (transaction->sent || response->request_sent) ? HTTP_SUBMISSION_SENT :
        (transaction->attempted || response->request_started) ? HTTP_SUBMISSION_ATTEMPTED : HTTP_SUBMISSION_NOT_STARTED,
    .http_status = response->status, .framing = response->state,
    .status = transaction->failure, .source_status = transaction->mutation_failure != CALL_OK ?
        transaction->mutation_failure : response->source_status,
    .cleanup_status = transaction->cleanup, .uploaded = transaction->uploaded,
    .received = transaction->charged.body_bytes, .consumed = transaction->consumed,
    .deadline_ns = transaction->deadline, .tls_error = response->tls_failure.error,
    .tls_native_status = response->tls_failure.native_status,
    .tls_cleanup_attempted = response->tls_cleanup_attempted,
    .response_flags = response->invalid_location ? HTTP_STATUS_INVALID_LOCATION : 0,
    .charged = {transaction->charged.header_bytes, transaction->charged.fields,
      transaction->charged.informational},
  };
  if (!transaction->response_charged) {
    if (response->upload_bytes > UINT64_MAX - status.uploaded ||
        response->consumed.body_bytes > UINT64_MAX - status.received) {
      reply(loop, packet, CALL_FILE_TOO_LARGE, NULL, 0, NULL, 0);
      return;
    }
    status.uploaded += response->upload_bytes;
    status.received += response->consumed.body_bytes;
    status.charged.header_bytes += response->consumed.header_bytes;
    status.charged.fields += response->consumed.fields;
    status.charged.informational += response->consumed.informational;
  }
  memcpy(status.media_type, response->media_type, sizeof(status.media_type));
  memcpy(status.location, response->location, sizeof(status.location));
  reply(loop, packet, CALL_OK, &status, sizeof(status), NULL, 0);
}

static void publish_metadata(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct http_response_metadata metadata = {0};
  memcpy(metadata.uri, transaction->uri, sizeof(metadata.uri));
  memcpy(metadata.media_type, transaction->response.media_type, sizeof(metadata.media_type));
  reply(loop, packet, CALL_OK, &metadata, sizeof(metadata), NULL, 0);
}

static void commit_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (transaction->handed_off ||
      (transaction->work && transaction->work->commit.receipt)) {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
  } else if (!transaction->work) {
    reply(loop, packet, transaction->failure == CALL_OK ? CALL_BAD_OPERATION : transaction->failure,
        NULL, 0, NULL, 0);
  } else if (transaction->phase != HTTP_TRANSACTION_STAGING) {
    reply(loop, packet, transaction->failure == CALL_OK ? CALL_BUSY : transaction->failure,
        NULL, 0, NULL, 0);
  } else if (transaction->mutation_failure != CALL_OK) {
    reply(loop, packet, transaction->mutation_failure, NULL, 0, NULL, 0);
    abort_transaction(loop, transaction, transaction->mutation_failure);
  } else {
    /* Freeze admission before observing another queued body operation. A
     * mutation already admitted remains owned until its completion is ACKed. */
    transaction->phase = HTTP_TRANSACTION_OPENING;
    release_body_source(loop, transaction);
    transaction->work->commit = *packet;
    clear_packet(packet);
  }
}

static void stage_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (transaction->phase != HTTP_TRANSACTION_STAGING || !transaction->body_source) {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
    return;
  }
  handle_t body = HANDLE_INVALID;
  enum call_status status = handle_copy_restricted(transaction->body_source,
      FILE_RIGHT_WRITE, HANDLE_TRANSPORT_CALL, &body);
  struct endpoint_grant grant = {body, FILE_RIGHT_WRITE, HANDLE_TRANSPORT_CALL};
  reply(loop, packet, status, NULL, 0, status == CALL_OK ? &grant : NULL,
      status == CALL_OK ? 1 : 0);
  if (body && handle_close(body)) {
    loop->fatal = CALL_BAD_HANDLE;
  }
}

static void body_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (!(packet->rights & FILE_RIGHT_WRITE)) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    return;
  }
  if (packet->operation == FILE_SIZE) {
    if (packet->size) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    } else if (transaction->mutation_failure != CALL_OK) {
      reply(loop, packet, transaction->mutation_failure, NULL, 0, NULL, 0);
    } else if (transaction->pending) {
      reply(loop, packet, CALL_BUSY, NULL, 0, NULL, 0);
    } else {
      struct file_size_reply response = {transaction->body_length};
      reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
    }
    return;
  }
  if (packet->operation == FILE_READ) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    return;
  }
  if (packet->operation != FILE_WRITE && packet->operation != FILE_RESIZE) {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
    return;
  }
  if (transaction->phase != HTTP_TRANSACTION_STAGING) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    return;
  }
  if (transaction->mutation_failure != CALL_OK || transaction->pending || transaction->work->mutation.receipt) {
    reply(loop, packet, transaction->mutation_failure != CALL_OK ?
        transaction->mutation_failure : CALL_BUSY, NULL, 0, NULL, 0);
    return;
  }
  enum staging_operation kind;
  if (packet->operation == FILE_WRITE) {
    struct file_write_request request;
    if (packet->size < sizeof(request)) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    memcpy(&request, packet->data, sizeof(request));
    if (request.size > FILE_WRITE_MAX_BYTES || request.size != packet->size - sizeof(request) ||
        request.offset > UINT64_MAX - request.size) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    if (!request.size) {
      struct file_write_reply response = {0};
      reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
      return;
    }
    transaction->mutation_offset = request.offset;
    transaction->mutation_size = request.size;
    kind = STAGING_WRITE;
  } else {
    struct file_resize_request request;
    if (packet->size != sizeof(request)) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    memcpy(&request, packet->data, sizeof(request));
    transaction->mutation_size = request.size;
    kind = STAGING_RESIZE;
  }
  uint64_t admitted_size = kind == STAGING_WRITE ?
      transaction->mutation_offset + transaction->mutation_size : transaction->mutation_size;
  if (loop->body_cap && admitted_size > loop->body_cap) {
    /* Refused growth never reaches native mutation. Remember the failure even
     * when a writer ignores its reply, so COMMIT cannot submit a partial body. */
    transaction->mutation_failure = CALL_LIMIT;
    reply(loop, packet, CALL_LIMIT, NULL, 0, NULL, 0);
    return;
  }
  if (!storage_reserve(loop, &transaction->body_charge, admitted_size)) {
    transaction->mutation_failure = CALL_FILE_TOO_LARGE;
    reply(loop, packet, CALL_FILE_TOO_LARGE, NULL, 0, NULL, 0);
    return;
  }
  transaction->staging_operation = kind;
  transaction->mutation_queued = true;
  transaction->work->mutation = *packet;
  clear_packet(packet);
}

static void staging_completion(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  enum staging_operation kind = transaction->staging_operation;
  transaction->pending = 0;
  loop->storage_owner = NULL;
  transaction->staging_operation = STAGING_NONE;
  enum call_status status = packet->result;
  if (kind == STAGING_CREATE) {
    if (status == CALL_OK && packet->grant_count == 1) {
      handle_t *target = transaction->creating_snapshot ? &transaction->snapshot : &transaction->backing;
      *target = packet->grants[0].handle;
      packet->grants[0].handle = HANDLE_INVALID;
      transaction->linked = true;
    } else if (status == CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  } else if (kind == STAGING_UNLINK) {
    if (status == CALL_OK) {
      transaction->linked = false;
    } else {
      transaction->cleanup = status;
    }
  } else if (kind == STAGING_WRITE || kind == SNAPSHOT_WRITE) {
    struct file_write_reply response;
    uint64_t requested = kind == SNAPSHOT_WRITE ? transaction->queued : transaction->mutation_size;
    if (status == CALL_OK && packet->size == sizeof(response) && !packet->grant_count) {
      memcpy(&response, packet->data, sizeof(response));
      if (response.written > requested || (requested && !response.written)) {
        status = CALL_OUTCOME_UNKNOWN;
      } else if (kind == SNAPSHOT_WRITE) {
        transaction->snapshot_size += response.written;
        transaction->queued -= response.written;
        if (transaction->queued) {
          memmove(transaction->queue, transaction->queue + response.written, transaction->queued);
        }
      } else if (response.written && transaction->mutation_offset + response.written > transaction->body_length) {
        transaction->body_length = transaction->mutation_offset + response.written;
      }
    } else if (status == CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (kind == STAGING_WRITE) {
      if (status != CALL_OK) {
        transaction->mutation_failure = status;
      } else {
        storage_release(loop, &transaction->body_charge, transaction->body_length);
      }
      if (transaction->work->mutation.receipt &&
          reply(loop, &transaction->work->mutation, status, status == CALL_OK ? packet->data : NULL,
              status == CALL_OK ? packet->size : 0, NULL, 0) != CALL_OK) {
        transaction->mutation_failure = CALL_OUTCOME_UNKNOWN;
      }
    } else if (status != CALL_OK) {
      abort_transaction(loop, transaction, status);
    }
  } else if (kind == STAGING_RESIZE) {
    if (status == CALL_OK && !packet->size && !packet->grant_count) {
      transaction->body_length = transaction->mutation_size;
      storage_release(loop, &transaction->body_charge, transaction->body_length);
    } else {
      status = status == CALL_OK ? CALL_OUTCOME_UNKNOWN : status;
      transaction->mutation_failure = status;
    }
    if (transaction->work->mutation.receipt &&
        reply(loop, &transaction->work->mutation, status, NULL, 0, NULL, 0) != CALL_OK) {
      transaction->mutation_failure = CALL_OUTCOME_UNKNOWN;
    }
  } else if (kind == SNAPSHOT_READ) {
    struct file_read_reply response;
    struct file_read_request request = {.offset = transaction->read_offset,
      .capacity = transaction->read_capacity};
    if (status == CALL_OK) {
      if (packet->size < sizeof(response) || packet->grant_count) {
        status = CALL_IO;
      } else {
        memcpy(&response, packet->data, sizeof(response));
        if (response.read > request.capacity || packet->size - sizeof(response) != response.read ||
            response.read > transaction->snapshot_size - request.offset || !response.read ||
            response.read > UINT64_MAX - transaction->consumed) {
          status = CALL_IO;
        } else {
          transaction->consumed += response.read;
        }
      }
    }
    if (transaction->work->read.receipt) {
      reply(loop, &transaction->work->read, status, status == CALL_OK ? packet->data : NULL,
          status == CALL_OK ? packet->size : 0, NULL, 0);
    }
  }
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (packet->grants[i].handle && handle_close(packet->grants[i].handle)) {
      transaction->cleanup = CALL_BAD_HANDLE;
    }
  }
  if (handle_close(packet->receipt)) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  clear_packet(packet);
  if (kind == STAGING_CREATE && transaction->linked) {
    enum call_status removed = unlink_backing(loop, transaction);
    if (removed != CALL_OK) {
      transaction->cleanup = removed;
      abort_transaction(loop, transaction, removed);
    }
  } else if ((kind == STAGING_CREATE || kind == STAGING_UNLINK) && status != CALL_OK) {
    abort_transaction(loop, transaction, status);
  } else if (kind == STAGING_UNLINK && !transaction->aborting) {
    if (transaction->creating_snapshot) {
      transaction->phase = HTTP_TRANSACTION_OPENING;
    } else {
      preparation_ready(loop, transaction);
    }
  }
}

static bool redirect_status(unsigned status)
{
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static void start_engine(struct loop *loop, struct transaction *transaction)
{
  if (transaction->mutation_failure != CALL_OK) {
    abort_transaction(loop, transaction, transaction->mutation_failure);
    return;
  }
  struct http_uri uri;
  enum call_status status = http_url_parse(transaction->work->uri, &uri);
  if (status != CALL_OK || (uri.scheme == HTTP_SCHEME_HTTPS && !loop->client.tls)) {
    abort_transaction(loop, transaction, status == CALL_OK ? CALL_UNAVAILABLE : status);
    return;
  }
  struct http_client client = loop->client;
  client.scheme = uri.scheme;
  struct http_request request = {.method = transaction->method,
    .body = transaction->method == HTTP_METHOD_POST ? transaction->backing : HANDLE_INVALID,
    .body_length = transaction->method == HTTP_METHOD_POST ? transaction->body_length : 0,
    .headers = transaction->work->headers_fields, .header_count = transaction->work->field_count};
  struct http_response_limits limits = {
    transaction->prepare_context.remaining.header_bytes,
    transaction->prepare_context.remaining.fields,
    transaction->prepare_context.remaining.informational,
  };
  transaction->engine = http_request_start(&client, transaction->work->uri, &request,
      transaction->deadline, &limits, loop->receiver, &loop->next_id, &transaction->response);
  if (!transaction->engine) {
    abort_transaction(loop, transaction, response_status(&transaction->response));
  }
}

static void snapshot_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (!(packet->rights & FILE_RIGHT_READ)) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
  } else if (transaction->failure != CALL_OK) {
    reply(loop, packet, transaction->failure, NULL, 0, NULL, 0);
  } else if (packet->operation == FILE_SIZE && !packet->size) {
    struct file_size_reply response = {transaction->snapshot_size};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
  } else if (packet->operation == FILE_INFO && !packet->size) {
    struct file_info_reply response = {.valid = FILE_INFO_SIZE_VALID | FILE_INFO_DOMAIN_VALID,
      .size = transaction->snapshot_size, .domain = FILE_DOMAIN_PRIVATE_BYTES};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
  } else if (packet->operation == FILE_READ && packet->size == sizeof(struct file_read_request)) {
    struct file_read_request request;
    memcpy(&request, packet->data, sizeof(request));
    if (request.capacity > FILE_READ_MAX_BYTES) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    } else if (transaction->work->read.receipt || transaction->pending) {
      reply(loop, packet, CALL_BUSY, NULL, 0, NULL, 0);
    } else if (!request.capacity || request.offset >= transaction->snapshot_size) {
      struct file_read_reply response = {0};
      reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
    } else {
      transaction->read_offset = request.offset;
      transaction->read_capacity = request.capacity;
      transaction->work->read = *packet;
      clear_packet(packet);
    }
  } else {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
  }
}


static bool credential(const struct http_request_header *header)
{
  return (header->name_length == 13 && !strncasecmp(header->name, "authorization", 13)) ||
      (header->name_length == 6 && !strncasecmp(header->name, "cookie", 6)) ||
      (header->name_length == 19 && !strncasecmp(header->name, "proxy-authorization", 19));
}

static enum call_status charge_hop(struct transaction *transaction)
{
  if (transaction->response_charged) {
    return CALL_OK;
  }
  struct provider_http_budget *used = &transaction->response.consumed;
  struct http_framing_budget *remaining = &transaction->prepare_context.remaining;
  if (used->header_bytes > remaining->header_bytes || used->fields > remaining->fields ||
      used->informational > remaining->informational ||
      used->body_bytes > UINT64_MAX - transaction->charged.body_bytes ||
      transaction->response.upload_bytes > UINT64_MAX - transaction->uploaded) {
    return CALL_FILE_TOO_LARGE;
  }
  remaining->header_bytes -= used->header_bytes;
  remaining->fields -= used->fields;
  remaining->informational -= used->informational;
  transaction->charged.body_bytes += used->body_bytes;
  transaction->charged.header_bytes += used->header_bytes;
  transaction->charged.fields += used->fields;
  transaction->charged.informational += used->informational;
  transaction->uploaded += transaction->response.upload_bytes;
  transaction->attempted |= transaction->response.request_started;
  transaction->sent |= transaction->response.request_sent;
  transaction->response_charged = true;
  return CALL_OK;
}

static enum call_status redirect_prepare(struct loop *loop, struct transaction *transaction)
{
  struct http_uri current, next;
  if (transaction->redirects == HTTP_REDIRECT_MAX || transaction->response.invalid_location ||
      !transaction->response.location[0]) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = http_url_parse(transaction->work->uri, &current);
  if (status == CALL_OK) {
    status = http_url_resolve(transaction->work->uri, transaction->response.location,
        transaction->work->next_uri);
  }
  if (status != CALL_OK) {
    snprintf(transaction->detail, sizeof(transaction->detail), "Redirect refused: %.200s",
        transaction->response.location);
    return status;
  }
  status = http_url_parse(transaction->work->next_uri, &next);
  if (status != CALL_OK) {
    return status;
  }
  bool crossing = current.scheme != next.scheme || current.port != next.port ||
      strcmp(current.host, next.host);
  if ((current.scheme == HTTP_SCHEME_HTTPS && next.scheme == HTTP_SCHEME_HTTP) ||
      (loop->custom_trust && crossing) ||
      (next.scheme == HTTP_SCHEME_HTTPS && !loop->client.tls)) {
    return CALL_DENIED;
  }
  status = http_url_key(transaction->work->next_uri, transaction->work->visited[transaction->redirects + 1]);
  if (status != CALL_OK) {
    return status;
  }
  for (size_t i = 0; i <= transaction->redirects; ++i) {
    if (!strcmp(transaction->work->visited[i], transaction->work->visited[transaction->redirects + 1])) {
      return CALL_LIMIT;
    }
  }
  bool get = transaction->response.status == 301 || transaction->response.status == 302 ||
      transaction->response.status == 303;
  size_t kept = 0;
  for (size_t i = 0; i < transaction->work->field_count; ++i) {
    struct http_request_header header = transaction->work->headers_fields[i];
    bool body_field = header.name_length == 12 && !strncasecmp(header.name, "content-type", 12);
    bool caller_field = (uintptr_t)header.value >= (uintptr_t)transaction->work->fields &&
        (uintptr_t)header.value < (uintptr_t)transaction->work->fields + sizeof(transaction->work->fields);
    if ((crossing && (credential(&header) ||
        (caller_field && (transaction->flags & HTTP_PREPARE_CREDENTIALS)))) || (get && body_field)) {
      /* Constant JSON defaults contain no secret storage. Parsed fields own writable values. */
      if (caller_field) {
        erase((void *)header.value, header.value_length);
      }
    } else {
      transaction->work->headers_fields[kept++] = header;
    }
  }
  transaction->work->field_count = kept;
  transaction->origin_crossed |= crossing;
  if (get) {
    transaction->method = HTTP_METHOD_GET;
  }
  transaction->redirecting = true;
  status = http_stream_abort(transaction->engine);
  return status;
}

static enum call_status final_http_status(unsigned code)
{
  if (code >= 200 && code < 300) {
    return CALL_OK;
  }
  if (code == 404 || code == 410) {
    return CALL_NOT_FOUND;
  }
  if (code == 401 || code == 403) {
    return CALL_DENIED;
  }
  return CALL_IO;
}

static void publish_snapshot(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct provider_open_reply response = {.description = transaction_description(transaction)};
  if (transaction->removing && transaction->failure == CALL_OK) {
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
    transaction->handed_off = true;
    return;
  }
  handle_t reader = HANDLE_INVALID;
  enum call_status status = export(loop, transaction, transaction->slot_count - 1,
      FACET_SNAPSHOT, &reader);
  if (status == CALL_OK) {
    status = publish_description(loop, transaction);
  }
  if (status == CALL_OK) {
    response.protocol = PROTOCOL_FILE;
    struct endpoint_grant grant = {reader, FILE_RIGHT_READ, HANDLE_TRANSPORT_CALL};
    status = reply(loop, packet, CALL_OK, &response, sizeof(response), &grant, 1);
  } else {
    reply(loop, packet, status, NULL, 0, NULL, 0);
  }
  if (reader && handle_close(reader)) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK) {
    transaction->handed_off = true;
  } else if (reader) {
    enum call_status cut = forwarder_cut(loop->exports[transaction->slots[transaction->slot_count - 1]].controller);
    if (cut != CALL_OK) {
      loop->fatal = cut;
    }
  }
}
static void settle(struct loop *loop, struct transaction *transaction)
{
  if (transaction->phase != HTTP_TRANSACTION_COMPLETE && !transaction->aborting) {
    return;
  }
  if (transaction->pending || (transaction->engine && http_stream_pending(transaction->engine))) {
    return;
  }
  if (transaction->engine) {
    enum call_status status = http_stream_close(transaction->engine);
    transaction->engine = NULL;
    if (status != CALL_OK) {
      transaction->cleanup = status;
      if (transaction->failure == CALL_OK) {
        transaction->failure = status;
      }
    }
  }
  if (transaction->linked && !transaction->unlink_attempted) {
    enum call_status status = unlink_backing(loop, transaction);
    if (status == CALL_OK) {
      return;
    }
    transaction->cleanup = status;
  }
  close_backing(loop, &transaction->backing, &transaction->body_charge);
  if (transaction->failure != CALL_OK) {
    close_backing(loop, &transaction->snapshot, &transaction->snapshot_charge);
  }
  if (transaction->work->preparation.receipt) {
    if (transaction->generic) {
      publish_snapshot(loop, transaction, &transaction->work->preparation);
    } else {
      reply(loop, &transaction->work->preparation,
          transaction->failure == CALL_OK ? CALL_IO : transaction->failure, NULL, 0, NULL, 0);
    }
  }
  if (transaction->work->commit.receipt) {
    publish_snapshot(loop, transaction, &transaction->work->commit);
  }
  if (!transaction->terminal_description_published) {
    enum call_status status = publish_description(loop, transaction);
    if (status != CALL_OK) {
      loop->fatal = status;
      return;
    }
    transaction->terminal_description_published = true;
  }
  if (transaction->work->mutation.receipt) {
    reply(loop, &transaction->work->mutation,
        transaction->failure == CALL_OK ? CALL_IO : transaction->failure, NULL, 0, NULL, 0);
    transaction->mutation_queued = false;
  }
  if (transaction->work->abort.receipt) {
    reply(loop, &transaction->work->abort, transaction->cleanup, NULL, 0, NULL, 0);
  }
  erase(transaction->work->fields, sizeof(transaction->work->fields));
  memset(transaction->work->headers_fields, 0, sizeof(transaction->work->headers_fields));
  transaction->work->field_count = 0;
  if (transaction->queue) {
    erase(transaction->queue, RESPONSE_BYTES);
    free(transaction->queue);
    transaction->queue = NULL;
  }
  if (!has_facet(loop, transaction, FACET_SNAPSHOT)) {
    close_backing(loop, &transaction->snapshot, &transaction->snapshot_charge);
  }
  if (transaction->failure == CALL_OK && transaction->error_body) {
    erase(transaction->error_body, transaction->error_size);
    free(transaction->error_body);
    transaction->error_body = NULL;
    transaction->error_size = 0;
  }
  if (loop->active == transaction) {
    loop->active = NULL;
  }
  destroy_if_retired(loop, transaction);
}

static bool progress(struct loop *loop)
{
  struct transaction *transaction = loop->active;
  if (!transaction) {
    return false;
  }
  if (transaction->phase == HTTP_TRANSACTION_COMPLETE) {
    transaction->wait = (struct http_wait){0};
    settle(loop, transaction);
    return false;
  }
  uint64_t now;
  enum call_status status = clock_now(loop->client.authority.clock, &now);
  if (status != CALL_OK || (now >= transaction->deadline && !transaction->aborting)) {
    abort_transaction(loop, transaction, status == CALL_OK ? CALL_TIMED_OUT : status);
  }
  if (transaction->aborting) {
    settle(loop, transaction);
    return false;
  }
  if (transaction->pending || loop->storage_owner) {
    return false;
  }
  if (transaction->mutation_queued) {
    struct endpoint_packet *packet = &transaction->work->mutation;
    status = submit(loop, transaction, transaction->backing, PROTOCOL_FILE, packet->operation,
        packet->data, packet->size, transaction->staging_operation, false);
    if (status == CALL_BUSY) {
      return false;
    }
    transaction->mutation_queued = false;
    if (status != CALL_OK) {
      transaction->mutation_failure = status;
      reply(loop, packet, status, NULL, 0, NULL, 0);
      abort_transaction(loop, transaction, status);
    }
    return true;
  }
  if (transaction->redirecting) {
    if (http_stream_pending(transaction->engine)) {
      return false;
    }
    status = charge_hop(transaction);
    enum call_status closed = http_stream_close(transaction->engine);
    transaction->engine = NULL;
    if (status == CALL_OK) {
      status = closed;
    }
    if (status != CALL_OK) {
      abort_transaction(loop, transaction, status);
      return true;
    }
    memcpy(transaction->work->uri, transaction->work->next_uri, sizeof(transaction->work->uri));
    memcpy(transaction->uri, transaction->work->uri, sizeof(transaction->uri));
    ++transaction->redirects;
    transaction->response = (struct http_response){0};
    transaction->response_charged = false;
    transaction->headers = false;
    transaction->redirecting = false;
    transaction->phase = HTTP_TRANSACTION_OPENING;
  }
  if (transaction->phase == HTTP_TRANSACTION_OPENING && !transaction->engine) {
    if (!transaction->removing && !transaction->snapshot) {
      status = create_backing(loop, transaction, true);
      if (status != CALL_OK && status != CALL_BUSY) {
        abort_transaction(loop, transaction, status);
      }
      return status != CALL_BUSY;
    }
    start_engine(loop, transaction);
    if (!transaction->engine) {
      settle(loop, transaction);
      return true;
    }
  }
  if (!transaction->engine) {
    return false;
  }
  if (transaction->queued) {
    unsigned char bytes[sizeof(struct file_write_request) + FILE_WRITE_MAX_BYTES];
    struct file_write_request request = {.offset = transaction->snapshot_size,
      .size = transaction->queued};
    memcpy(bytes, &request, sizeof(request));
    memcpy(bytes + sizeof(request), transaction->queue, transaction->queued);
    status = submit(loop, transaction, transaction->snapshot, PROTOCOL_FILE, FILE_WRITE,
        bytes, sizeof(request) + transaction->queued, SNAPSHOT_WRITE, false);
    if (status != CALL_OK && status != CALL_BUSY) {
      abort_transaction(loop, transaction, status);
    }
    return status != CALL_BUSY;
  }
  size_t count = 0;
  enum http_step_result step = http_stream_step(transaction->engine,
      transaction->queue, FILE_WRITE_MAX_BYTES, &count, &transaction->wait);
  if (step == HTTP_STEP_HEADERS) {
    transaction->headers = true;
    if (redirect_status(transaction->response.status)) {
      status = redirect_prepare(loop, transaction);
      if (status != CALL_OK) {
        abort_transaction(loop, transaction, status);
      }
    } else if (!transaction->removing && final_http_status(transaction->response.status) == CALL_OK &&
        transaction->response.length_present &&
        !storage_reserve(loop, &transaction->snapshot_charge, transaction->response.length)) {
      abort_transaction(loop, transaction, CALL_FILE_TOO_LARGE);
    } else {
      transaction->phase = HTTP_TRANSACTION_READING;
    }
  } else if (step == HTTP_STEP_DATA) {
    if (transaction->error_size < RESOURCE_ERROR_BODY_MAX_BYTES && count) {
      if (!transaction->error_body) {
        transaction->error_body = malloc(RESOURCE_ERROR_BODY_MAX_BYTES);
      }
      if (!transaction->error_body) {
        abort_transaction(loop, transaction, CALL_NO_MEMORY);
      } else {
        size_t keep = RESOURCE_ERROR_BODY_MAX_BYTES - transaction->error_size;
        if (keep > count) {
          keep = count;
        }
        memcpy(transaction->error_body + transaction->error_size, transaction->queue, keep);
        transaction->error_size += keep;
      }
    }
    if (!transaction->aborting && !transaction->removing &&
        final_http_status(transaction->response.status) == CALL_OK) {
      if (count > UINT64_MAX - transaction->snapshot_size ||
          !storage_reserve(loop, &transaction->snapshot_charge, transaction->snapshot_size + count)) {
        abort_transaction(loop, transaction, CALL_FILE_TOO_LARGE);
      } else {
        transaction->queued = count;
      }
    }
  } else if (step == HTTP_STEP_COMPLETE) {
    status = charge_hop(transaction);
    if (status == CALL_OK) {
      transaction->failure = final_http_status(transaction->response.status);
      transaction->phase = HTTP_TRANSACTION_COMPLETE;
    } else {
      abort_transaction(loop, transaction, status);
    }
  } else if (step == HTTP_STEP_FAILED) {
    abort_transaction(loop, transaction, response_status(&transaction->response));
  }
  settle(loop, transaction);
  return step != HTTP_STEP_WAIT;
}

static bool progress_reads(struct loop *loop)
{
  if (loop->storage_owner || (loop->active && loop->active->engine &&
      http_stream_pending(loop->active->engine))) {
    return false;
  }
  for (size_t i = 0; i < EXPORT_SLOTS; ++i) {
    struct transaction *transaction = loop->exports[i].owner;
    if (!transaction || !transaction->work->read.receipt || transaction->pending) {
      continue;
    }
    struct endpoint_packet *packet = &transaction->work->read;
    enum call_status status = submit(loop, transaction, transaction->snapshot,
        PROTOCOL_FILE, FILE_READ, packet->data, packet->size, SNAPSHOT_READ, true);
    if (status != CALL_OK) {
      reply(loop, packet, status, NULL, 0, NULL, 0);
    }
    return true;
  }
  return false;
}

static void canceled(struct loop *loop, handle_t receipt)
{
  struct transaction *transaction = loop->active;
  if (transaction) {
    if (transaction->work->preparation.receipt == receipt) {
      finish(loop, &transaction->work->preparation);
      abort_transaction(loop, transaction, CALL_ABANDONED);
      return;
    }
    if (transaction->work->commit.receipt == receipt) {
      finish(loop, &transaction->work->commit);
      abort_transaction(loop, transaction, transaction->response.request_started ?
          CALL_OUTCOME_UNKNOWN : CALL_ABANDONED);
      return;
    }
    if (transaction->work->abort.receipt == receipt) {
      finish(loop, &transaction->work->abort);
      return;
    }
    if (transaction->work->mutation.receipt == receipt) {
      finish(loop, &transaction->work->mutation);
      transaction->mutation_failure = CALL_OUTCOME_UNKNOWN;
      abort_transaction(loop, transaction, CALL_OUTCOME_UNKNOWN);
      return;
    }
  }
  for (size_t i = 0; i < EXPORT_SLOTS; ++i) {
    transaction = loop->exports[i].owner;
    if (transaction && transaction->work && transaction->work->read.receipt == receipt) {
      /* This deferred read has not consumed bytes yet. */
      finish(loop, &transaction->work->read);
      return;
    }
  }
  if (endpoint_finish(receipt) != CALL_OK) {
    loop->fatal = CALL_BAD_HANDLE;
  }
}

static void retired(struct loop *loop, const struct endpoint_packet *packet)
{
  if (packet->protocol == PROTOCOL_PROVIDER && packet->object_id >= HTTPFS_ROOT_OBJECT_BASE &&
      packet->object_id < HTTPFS_TRANSACTION_OBJECT_BASE) {
    size_t root = packet->object_id - HTTPFS_ROOT_OBJECT_BASE;
    if (!loop->roots[root] || endpoint_retire_ack(loop->receiver, packet->object_id) != CALL_OK ||
        handle_close(loop->roots[root])) {
      loop->fatal = CALL_BAD_HANDLE;
      return;
    }
    loop->roots[root] = HANDLE_INVALID;
    return;
  }
  if (packet->object_id < HTTPFS_TRANSACTION_OBJECT_BASE ||
      packet->object_id - HTTPFS_TRANSACTION_OBJECT_BASE >= EXPORT_SLOTS) {
    loop->fatal = CALL_BAD_REQUEST;
    return;
  }
  struct exported *entry = &loop->exports[packet->object_id - HTTPFS_TRANSACTION_OBJECT_BASE];
  struct transaction *transaction = entry->owner;
  if (!entry->live || !transaction || packet->protocol != facet_protocol(entry->facet)) {
    loop->fatal = CALL_BAD_REQUEST;
    return;
  }
  enum facet facet = entry->facet;
  if (facet == FACET_CONTROL) {
    release_body_source(loop, transaction);
  }
  enum call_status status = endpoint_retire_ack(loop->receiver, packet->object_id);
  if (status != CALL_OK) {
    loop->fatal = status;
    return;
  }
  if (handle_close(entry->controller)) {
    loop->fatal = CALL_BAD_HANDLE;
    return;
  }
  entry->controller = HANDLE_INVALID;
  entry->live = false;
  if (facet == FACET_CONTROL && !transaction->handed_off && !transaction->work->commit.receipt) {
    abort_transaction(loop, transaction, CALL_ABANDONED);
  }
  settle(loop, transaction);
  if (entry->owner) {
    destroy_if_retired(loop, entry->owner);
  }
}

static void receive_packet(struct loop *loop, struct endpoint_packet *packet)
{
  if (packet->kind == ENDPOINT_MESSAGE_COMPLETION) {
    struct transaction *transaction = loop->active;
    if (loop->storage_owner && loop->storage_owner->pending == packet->correlation_id) {
      struct transaction *owner = loop->storage_owner;
      staging_completion(loop, owner, packet);
      settle(loop, owner);
    } else if (transaction && transaction->engine && http_stream_completion(transaction->engine, packet)) {
      clear_packet(packet);
    } else {
      loop->fatal = CALL_BAD_REQUEST;
    }
    return;
  }
  if (packet->kind == ENDPOINT_MESSAGE_RETIRE) {
    retired(loop, packet);
    return;
  }
  if (packet->kind == ENDPOINT_MESSAGE_CANCEL) {
    canceled(loop, packet->receipt);
    return;
  }
  bool attachments = packet->grant_count != 0;
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (handle_close(packet->grants[i].handle)) {
      loop->fatal = CALL_BAD_HANDLE;
    }
  }
  if (packet->kind == ENDPOINT_MESSAGE_SEND) {
    finish(loop, packet);
    return;
  }
  if (attachments) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  if (packet->object_id >= HTTPFS_ROOT_OBJECT_BASE &&
      packet->object_id < HTTPFS_TRANSACTION_OBJECT_BASE && packet->protocol == PROTOCOL_PROVIDER &&
      loop->roots[packet->object_id - HTTPFS_ROOT_OBJECT_BASE]) {
    if (packet->operation == PROVIDER_PREPARE) {
      prepare(loop, packet->object_id - HTTPFS_ROOT_OBJECT_BASE, packet);
    } else if (packet->operation == PROVIDER_OPEN || packet->operation == PROVIDER_REMOVE) {
      open_resource(loop, packet->object_id - HTTPFS_ROOT_OBJECT_BASE, packet);
    } else {
      reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
    }
    return;
  }
  if (packet->object_id < HTTPFS_TRANSACTION_OBJECT_BASE ||
      packet->object_id - HTTPFS_TRANSACTION_OBJECT_BASE >= EXPORT_SLOTS) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  struct exported *entry = &loop->exports[packet->object_id - HTTPFS_TRANSACTION_OBJECT_BASE];
  struct transaction *transaction = entry->owner;
  if (!entry->live || !transaction || packet->protocol != facet_protocol(entry->facet)) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
  } else if (entry->facet == FACET_BODY) {
    body_request(loop, transaction, packet);
  } else if (entry->facet == FACET_SNAPSHOT) {
    snapshot_request(loop, transaction, packet);
  } else if (packet->size) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
  } else if (entry->facet == FACET_OBSERVATION) {
    if (packet->operation == HTTP_TRANSACTION_STATUS && (packet->rights & HTTP_TRANSACTION_RIGHT_STATUS)) {
      publish_status(loop, transaction, packet);
    } else if (packet->operation == HTTP_TRANSACTION_METADATA &&
        (packet->rights & HTTP_TRANSACTION_RIGHT_STATUS)) {
      publish_metadata(loop, transaction, packet);
    } else {
      reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    }
  } else if (entry->facet == FACET_CONTROL) {
    if (packet->operation == TRANSACTION_STAGE && (packet->rights & TRANSACTION_RIGHT_STAGE)) {
      stage_request(loop, transaction, packet);
    } else if (packet->operation == TRANSACTION_COMMIT && (packet->rights & TRANSACTION_RIGHT_COMMIT)) {
      commit_request(loop, transaction, packet);
    } else if (packet->operation == TRANSACTION_ABORT && (packet->rights & TRANSACTION_RIGHT_ABORT)) {
      if (loop->active != transaction && !transaction->pending &&
          (!transaction->engine || !http_stream_pending(transaction->engine))) {
        reply(loop, packet, transaction->cleanup, NULL, 0, NULL, 0);
      } else if (transaction->work && transaction->work->abort.receipt) {
        reply(loop, packet, CALL_OK, NULL, 0, NULL, 0);
      } else {
        abort_transaction(loop, transaction, CALL_ABANDONED);
        if (transaction->work) {
          /* Redirect callers cannot reuse the transfer slot until cancellation
           * and stream cleanup have relinquished every pending operation. */
          transaction->work->abort = *packet;
          clear_packet(packet);
        } else {
          reply(loop, packet, transaction->cleanup, NULL, 0, NULL, 0);
        }
      }
    } else {
      reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    }
  } else {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
  }
}

static bool alive(const struct loop *loop)
{
  if (loop->active || loop->storage_owner) {
    return true;
  }
  for (size_t i = 0; i < HTTPFS_ROOT_COUNT; ++i) {
    if (loop->roots[i]) {
      return true;
    }
  }
  for (size_t i = 0; i < EXPORT_SLOTS; ++i) {
    if (loop->exports[i].owner) {
      return true;
    }
  }
  return false;
}

enum call_status httpfs_serve(handle_t service, handle_t receiver,
    handle_t root_controllers[HTTPFS_ROOT_COUNT], const struct http_client *client,
    bool custom_trust, handle_t staging, uint64_t body_cap, uint64_t storage_cap)
{
  struct loop loop = {.service = service, .receiver = receiver,
    .client = *client, .next_id = 1, .staging = staging, .body_cap = body_cap,
    .storage_cap = storage_cap, .custom_trust = custom_trust};
  for (size_t i = 0; i < HTTPFS_ROOT_COUNT; ++i) {
    loop.roots[i] = root_controllers[i];
    root_controllers[i] = HANDLE_INVALID;
  }
  while (loop.fatal == CALL_OK && alive(&loop)) {
    struct endpoint_packet packet = {0};
    enum call_status status = endpoint_try_receive(receiver, &packet);
    bool received = status == CALL_OK;
    if (received) {
      receive_packet(&loop, &packet);
    } else if (status != CALL_WOULD_BLOCK) {
      loop.fatal = status;
      break;
    }
    bool advanced = progress_reads(&loop);
    advanced = progress(&loop) || advanced;
    if (loop.fatal != CALL_OK || !alive(&loop) || advanced || received) {
      continue;
    }
    /* TRY consumes no empty queue. Registration-before-scan in wait_many
     * observes arrivals between this collection attempt and parking. */
    struct wait_interest interests[2] = {{.handle = receiver, .events = WAIT_READABLE}};
    uint64_t events[2] = {0};
    size_t count = 1;
    struct transaction *transaction = loop.active;
    if (transaction && !transaction->aborting && transaction->wait.stream && transaction->wait.events) {
      interests[count++] = (struct wait_interest){.handle = transaction->wait.stream, .events = transaction->wait.events};
    }
    uint64_t now;
    status = clock_now(client->authority.clock, &now);
    if (status != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
      loop.fatal = status == CALL_OK ? CALL_LIMIT : status;
      break;
    }
    uint64_t deadline = now + WAIT_MAX_WAIT_NS;
    if (transaction && !transaction->aborting && transaction->deadline < deadline) {
      deadline = transaction->deadline;
    }
    status = wait_many(interests, count, deadline, events);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      loop.fatal = status;
    }
  }
  /* Fatal provider failure is process death: native completion receiver close
   * drains kernel ownership independently; every exported facet fails closed. */
  for (size_t i = 0; i < HTTPFS_ROOT_COUNT; ++i) {
    if (loop.roots[i] && handle_close(loop.roots[i])) {
      loop.fatal = CALL_BAD_HANDLE;
    }
  }
  for (size_t i = 0; i < EXPORT_SLOTS; ++i) {
    if (loop.exports[i].controller && handle_close(loop.exports[i].controller) != 0) {
      loop.fatal = CALL_BAD_HANDLE;
    }
  }
  return loop.fatal;
}
