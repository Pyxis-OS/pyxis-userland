#include "provider.h"
#include <abi/completion.h>
#include <abi/directory.h>
#include <abi/http_transaction.h>
#include <clock.h>
#include <completion.h>
#include <directory.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wait.h>

#define SERVICE_ID UINT64_C(1)
#define EXPORT_SLOTS (ENDPOINT_EXPORTS_MAX - 1)
#define RESPONSE_BYTES 8192u
#define REQUEST_FIELDS_MAX 256u

enum facet { FACET_RESERVED, FACET_CONTROL, FACET_BODY, FACET_OBSERVATION,
  FACET_RESPONSE, FACET_SNAPSHOT };
enum staging_operation { STAGING_NONE, STAGING_CREATE, STAGING_UNLINK,
  STAGING_WRITE, STAGING_RESIZE };

struct transaction;
struct exported {
  struct transaction *owner;
  enum facet facet;
  bool live;
};

struct transaction_work {
  struct endpoint_packet preparation, commit, mutation, read;
  char uri[HTTP_URI_MAX + 1], name[64];
  char fields[ENDPOINT_DATA_MAX];
  struct http_request_header headers_fields[REQUEST_FIELDS_MAX];
  size_t field_count;
};

struct transaction {
  struct transaction_work *work;
  struct http_response response;
  struct http_stream *engine;
  struct http_wait wait;
  struct http_body snapshot;
  struct provider_open_request context;
  enum http_transaction_phase phase;
  enum call_status failure, cleanup, mutation_failure, read_failure;
  bool legacy, headers, handed_off, aborting, linked, unlink_attempted;
  uint64_t method, flags, deadline, body_length, consumed;
  uint64_t mutation_offset, mutation_size, pending;
  enum staging_operation staging_operation;
  handle_t backing;
  size_t slots[5], slot_count;
  size_t queued;
  unsigned char *queue;
};

struct loop {
  handle_t service, receiver, staging;
  struct http_client client;
  struct http_storage storage;
  struct exported exports[EXPORT_SLOTS];
  struct transaction *active;
  uint64_t next_id, next_name;
  bool published, custom_trust;
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
  return slot + 2;
}

static uint64_t facet_protocol(enum facet facet)
{
  return facet == FACET_BODY || facet == FACET_SNAPSHOT ? PROTOCOL_FILE :
      facet == FACET_RESPONSE ? PROTOCOL_STREAM : PROTOCOL_HTTP_TRANSACTION;
}

static uint64_t facet_rights(enum facet facet)
{
  switch (facet) {
  case FACET_BODY: return FILE_RIGHT_WRITE;
  case FACET_SNAPSHOT: return FILE_RIGHT_READ;
  case FACET_RESPONSE: return STREAM_RIGHT_READ;
  case FACET_CONTROL: return HTTP_TRANSACTION_CONTROL_RIGHTS;
  case FACET_OBSERVATION: return HTTP_TRANSACTION_RIGHT_STATUS;
  default: return 0;
  }
}

static enum call_status export(struct loop *loop, struct transaction *transaction,
    size_t position, enum facet facet, handle_t *handle)
{
  size_t slot = transaction->slots[position];
  enum call_status status = endpoint_export(loop->service, loop->receiver, object_id(slot),
      facet_protocol(facet), facet_rights(facet), HANDLE_TRANSPORT_CALL, handle);
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

static void withdraw(struct loop *loop, struct transaction *transaction)
{
  for (size_t i = 0; i < transaction->slot_count; ++i) {
    struct exported *entry = &loop->exports[transaction->slots[i]];
    if (entry->owner == transaction && entry->live) {
      enum call_status status = endpoint_withdraw(loop->receiver, object_id(transaction->slots[i]));
      if (status != CALL_OK && status != CALL_NOT_FOUND) {
        loop->fatal = status;
      }
    }
  }
}

static void destroy_if_retired(struct loop *loop, struct transaction *transaction)
{
  if (transaction->engine || transaction->pending || (transaction->work &&
      (transaction->work->preparation.receipt || transaction->work->commit.receipt ||
      transaction->work->mutation.receipt || transaction->work->read.receipt))) {
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
  http_body_release(&transaction->snapshot);
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

enum call_status httpfs_staging_validate(handle_t staging)
{
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
  return info.type == FILESYSTEM_TYPE_NPFS && !(info.flags & FILESYSTEM_FLAG_READ_ONLY) ? CALL_OK : CALL_DENIED;
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
    bool content_type = false;
    for (size_t i = 0; i < transaction->work->field_count; ++i) {
      struct http_request_header *header = &transaction->work->headers_fields[i];
      content_type |= header->name_length == 12 && !strncasecmp(header->name, "content-type", 12);
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
  enum call_status status = CALL_OK;
  for (size_t i = 0; i < count && status == CALL_OK; ++i) {
    status = export(loop, transaction, i, facets[i], &handles[i]);
  }
  if (status == CALL_OK) {
    struct endpoint_grant grants[3];
    for (size_t i = 0; i < count; ++i) {
      grants[i] = (struct endpoint_grant){handles[i], facet_rights(facets[i]), HANDLE_TRANSPORT_CALL};
    }
    struct http_prepare_reply response = {transaction->method};
    status = reply(loop, &transaction->work->preparation, CALL_OK, &response, sizeof(response), grants, count);
  } else {
    reply(loop, &transaction->work->preparation, status, NULL, 0, NULL, 0);
  }
  for (size_t i = 0; i < count; ++i) {
    if (handles[i] && handle_close(handles[i])) {
      loop->fatal = CALL_BAD_HANDLE;
    }
  }
  if (status != CALL_OK) {
    abort_transaction(loop, transaction, status);
    withdraw(loop, transaction);
  } else {
    transaction->phase = HTTP_TRANSACTION_STAGING;
  }
}

static void prepare(struct loop *loop, struct endpoint_packet *packet)
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
  status = reserve(loop, request.method == HTTP_PREPARE_POST ? 5 : 4, &transaction);
  if (status != CALL_OK) {
    reply(loop, packet, status, NULL, 0, NULL, 0);
    return;
  }
  transaction->method = request.method;
  transaction->flags = request.flags;
  transaction->deadline = request.deadline_ns;
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
    memcpy(transaction->work->fields, packet->data + sizeof(request) + request.uri_size, request.headers_size);
    if (!parse_fields(transaction, request.headers_size)) {
      status = CALL_BAD_REQUEST;
    } else {
      struct http_request validation = {.method = request.method,
        .headers = transaction->work->headers_fields, .header_count = transaction->work->field_count};
      if (!http_request_validate(&loop->client, transaction->work->uri, &validation, &transaction->response)) {
        status = response_status(&transaction->response);
      }
    }
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
  if (loop->next_name == UINT64_MAX) {
    status = CALL_LIMIT;
  } else {
    snprintf(transaction->work->name, sizeof(transaction->work->name), "http-stage-%llu", (unsigned long long)++loop->next_name);
    size_t length = strlen(transaction->work->name);
    unsigned char bytes[sizeof(struct completion_create_request) + sizeof(transaction->work->name)];
    struct completion_create_request creation = {.name_length = length,
      .kind = DIRECTORY_KIND_FILE, .rights = FILE_RIGHTS};
    memcpy(bytes, &creation, sizeof(creation));
    memcpy(bytes + sizeof(creation), transaction->work->name, length);
    status = submit(loop, transaction, loop->staging, PROTOCOL_DIRECTORY, DIRECTORY_CREATE,
        bytes, sizeof(creation) + length, STAGING_CREATE, false);
  }
  if (status != CALL_OK) {
    reply(loop, &transaction->work->preparation, status, NULL, 0, NULL, 0);
    abort_transaction(loop, transaction, status);
  }
}

static void publish_status(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct http_response *response = &transaction->response;
  struct http_transaction_status status = {
    .phase = transaction->phase,
    .submission = response->request_sent ? HTTP_SUBMISSION_SENT :
        response->request_started ? HTTP_SUBMISSION_ATTEMPTED : HTTP_SUBMISSION_NOT_STARTED,
    .http_status = response->status, .framing = response->state,
    .status = transaction->failure, .source_status = transaction->mutation_failure != CALL_OK ?
        transaction->mutation_failure : response->source_status,
    .cleanup_status = transaction->cleanup, .uploaded = response->upload_bytes,
    .received = response->consumed.body_bytes, .consumed = transaction->consumed,
    .deadline_ns = transaction->deadline, .tls_error = response->tls_failure.error,
    .tls_native_status = response->tls_failure.native_status,
    .tls_cleanup_attempted = response->tls_cleanup_attempted,
  };
  memcpy(status.media_type, response->media_type, sizeof(status.media_type));
  memcpy(status.location, response->location, sizeof(status.location));
  reply(loop, packet, CALL_OK, &status, sizeof(status), NULL, 0);
}

static void commit_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (transaction->handed_off ||
      (transaction->work && transaction->work->commit.receipt)) {
    struct http_commit_reply response = {.http_status = transaction->response.status};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
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
    transaction->work->commit = *packet;
    clear_packet(packet);
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
  if (transaction->mutation_failure != CALL_OK || transaction->pending) {
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
  enum call_status status = submit(loop, transaction, transaction->backing,
      PROTOCOL_FILE, packet->operation, packet->data, packet->size, kind, false);
  if (status != CALL_OK) {
    transaction->mutation_failure = status;
    reply(loop, packet, status, NULL, 0, NULL, 0);
  } else {
    transaction->work->mutation = *packet;
    clear_packet(packet);
  }
}

static void staging_completion(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  enum staging_operation kind = transaction->staging_operation;
  transaction->pending = 0;
  transaction->staging_operation = STAGING_NONE;
  enum call_status status = packet->result;
  if (kind == STAGING_CREATE) {
    if (status == CALL_OK && packet->grant_count == 1) {
      transaction->backing = packet->grants[0].handle;
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
  } else if (kind == STAGING_WRITE) {
    struct file_write_reply response;
    if (status == CALL_OK && packet->size == sizeof(response)) {
      memcpy(&response, packet->data, sizeof(response));
      if (response.written > transaction->mutation_size ||
          (transaction->mutation_size && !response.written)) {
        status = CALL_OUTCOME_UNKNOWN;
      } else if (response.written && transaction->mutation_offset + response.written > transaction->body_length) {
        transaction->body_length = transaction->mutation_offset + response.written;
      }
    } else if (status == CALL_OK) {
      status = CALL_OUTCOME_UNKNOWN;
    }
    if (status != CALL_OK) {
      transaction->mutation_failure = status;
    }
    if (transaction->work->mutation.receipt) {
      enum call_status sent = reply(loop, &transaction->work->mutation, status,
          status == CALL_OK ? packet->data : NULL, status == CALL_OK ? packet->size : 0, NULL, 0);
      if (sent != CALL_OK) {
        transaction->mutation_failure = CALL_OUTCOME_UNKNOWN;
      }
    }
  } else if (kind == STAGING_RESIZE) {
    if (status == CALL_OK && !packet->size) {
      transaction->body_length = transaction->mutation_size;
    } else {
      status = status == CALL_OK ? CALL_OUTCOME_UNKNOWN : status;
      transaction->mutation_failure = status;
    }
    if (transaction->work->mutation.receipt &&
        reply(loop, &transaction->work->mutation, status, NULL, 0, NULL, 0) != CALL_OK) {
      transaction->mutation_failure = CALL_OUTCOME_UNKNOWN;
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
    preparation_ready(loop, transaction);
  }
}

static void deliver_read(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  if (!packet->receipt) {
    return;
  }
  if (transaction->read_failure != CALL_OK) {
    reply(loop, packet, transaction->read_failure, NULL, 0, NULL, 0);
    return;
  }
  struct stream_read_request request;
  memcpy(&request, packet->data, sizeof(request));
  if (transaction->queued || !request.capacity) {
    size_t size = transaction->queued < request.capacity ? transaction->queued : request.capacity;
    unsigned char bytes[ENDPOINT_DATA_MAX];
    struct stream_read_reply response = {size};
    memcpy(bytes, &response, sizeof(response));
    if (size) {
      memcpy(bytes + sizeof(response), transaction->queue, size);
    }
    transaction->queued -= size;
    if (transaction->queued) {
      memmove(transaction->queue, transaction->queue + size, transaction->queued);
    }
    transaction->consumed += size;
    if (reply(loop, packet, CALL_OK, bytes, sizeof(response) + size, NULL, 0) != CALL_OK) {
      transaction->read_failure = CALL_OUTCOME_UNKNOWN;
      transaction->queued = 0;
      abort_transaction(loop, transaction, CALL_OUTCOME_UNKNOWN);
    }
    erase(bytes, sizeof(response) + size);
  } else if (transaction->phase == HTTP_TRANSACTION_COMPLETE) {
    struct stream_read_reply response = {0};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
  } else if (transaction->phase == HTTP_TRANSACTION_FAILED || transaction->phase == HTTP_TRANSACTION_ABORTED) {
    reply(loop, packet, transaction->failure, NULL, 0, NULL, 0);
  }
}

static void response_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct stream_read_request request;
  if (!(packet->rights & STREAM_RIGHT_READ)) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
  } else if (packet->operation != STREAM_READ) {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
  } else if (packet->size != sizeof(request)) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
  } else {
    memcpy(&request, packet->data, sizeof(request));
    if (!request.capacity || request.capacity > STREAM_READ_MAX_BYTES) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    } else if (!transaction->work) {
      deliver_read(loop, transaction, packet);
    } else if (transaction->work->read.receipt) {
      reply(loop, packet, CALL_BUSY, NULL, 0, NULL, 0);
    } else {
      transaction->work->read = *packet;
      clear_packet(packet);
      deliver_read(loop, transaction, &transaction->work->read);
    }
  }
}

static bool redirect_status(unsigned status)
{
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static enum call_status legacy_context(struct loop *loop,
    struct provider_open_request *request, const char *text)
{
  struct provider_http_origin zero_origin = {0};
  struct provider_http_budget zero_budget = {0};
  if ((request->flags & ~PROVIDER_OPEN_FLAGS) ||
      ((request->flags & PROVIDER_OPEN_ORIGIN_CROSSED) && !(request->flags & PROVIDER_OPEN_HTTP))) {
    return CALL_BAD_REQUEST;
  }
  struct http_uri uri;
  enum call_status status = http_url_parse(text, &uri);
  if (status != CALL_OK || uri.scheme != loop->client.scheme) {
    return status == CALL_OK ? CALL_BAD_OPERATION : status;
  }
  if (!(request->flags & PROVIDER_OPEN_HTTP)) {
    if (memcmp(&request->origin, &zero_origin, sizeof(zero_origin)) ||
        memcmp(&request->remaining, &zero_budget, sizeof(zero_budget))) {
      return CALL_BAD_REQUEST;
    }
    request->flags = PROVIDER_OPEN_HTTP;
    request->origin.scheme = uri.scheme;
    request->origin.port = uri.port;
    memcpy(request->origin.host, uri.host, strlen(uri.host) + 1);
    request->remaining = (struct provider_http_budget){HTTP_BODY_MAX,
      HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX};
  } else {
    size_t length = strnlen(request->origin.host, sizeof(request->origin.host));
    if (!length || length == sizeof(request->origin.host) ||
        request->origin.scheme > PROVIDER_HTTPS || !request->origin.port ||
        request->origin.port > UINT16_MAX || request->remaining.body_bytes > HTTP_BODY_MAX ||
        request->remaining.header_bytes > HTTP_HEADERS_MAX || request->remaining.fields > HTTP_FIELDS_MAX ||
        request->remaining.informational > HTTP_INFORMATIONAL_MAX) {
      return CALL_BAD_REQUEST;
    }
    for (size_t i = length + 1; i < sizeof(request->origin.host); ++i) {
      if (request->origin.host[i]) {
        return CALL_BAD_REQUEST;
      }
    }
    char origin_text[HTTP_URI_MAX + 1];
    int size = snprintf(origin_text, sizeof(origin_text), "%s://%s:%llu/",
        request->origin.scheme == PROVIDER_HTTPS ? "https" : "http", request->origin.host,
        (unsigned long long)request->origin.port);
    struct http_uri origin;
    if (size < 0 || (size_t)size >= sizeof(origin_text) ||
        http_url_parse(origin_text, &origin) != CALL_OK ||
        origin.scheme != request->origin.scheme || origin.port != request->origin.port ||
        strcmp(origin.host, request->origin.host)) {
      return CALL_BAD_REQUEST;
    }
  }
  if (loop->custom_trust && ((request->flags & PROVIDER_OPEN_ORIGIN_CROSSED) ||
      request->origin.scheme != uri.scheme || request->origin.port != uri.port ||
      strcmp(request->origin.host, uri.host))) {
    return CALL_DENIED;
  }
  return CALL_OK;
}

static void legacy_error(struct loop *loop, struct endpoint_packet *packet,
    enum call_status status, unsigned http_status)
{
  struct provider_open_reply response = {.provider_status = http_status};
  reply(loop, packet, status, &response, sizeof(response), NULL, 0);
}

static void legacy_open(struct loop *loop, struct endpoint_packet *packet)
{
  struct provider_open_request request;
  if (packet->size < sizeof(request)) {
    legacy_error(loop, packet, CALL_BAD_REQUEST, 0);
    return;
  }
  memcpy(&request, packet->data, sizeof(request));
  if (request.rights != FILE_RIGHT_READ || !(packet->rights & PROVIDER_RIGHT_OPEN_READ)) {
    legacy_error(loop, packet, CALL_DENIED, 0);
    return;
  }
  if (!request.uri_size || request.uri_size > HTTP_URI_MAX ||
      packet->size - sizeof(request) != request.uri_size ||
      memchr(packet->data + sizeof(request), 0, request.uri_size)) {
    legacy_error(loop, packet, CALL_BAD_REQUEST, 0);
    return;
  }
  struct transaction *transaction;
  enum call_status status = reserve(loop, 1, &transaction);
  if (status != CALL_OK) {
    legacy_error(loop, packet, status == CALL_LIMIT ? CALL_QUEUE_FULL : status, 0);
    return;
  }
  transaction->legacy = true;
  memcpy(transaction->work->uri, packet->data + sizeof(request), request.uri_size);
  status = legacy_context(loop, &request, transaction->work->uri);
  transaction->context = request;
  if (status == CALL_OK) {
    uint64_t now;
    status = clock_now(loop->client.authority.clock, &now);
    if (status == CALL_OK && now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
      status = CALL_LIMIT;
    }
    if (status == CALL_OK) {
      transaction->deadline = now + WAIT_MAX_WAIT_NS;
      if (packet->deadline_ns && packet->deadline_ns < transaction->deadline) {
        transaction->deadline = packet->deadline_ns;
      }
      if (transaction->deadline <= now) {
        status = CALL_TIMED_OUT;
      }
    }
  }
  if (status != CALL_OK) {
    legacy_error(loop, packet, status, 0);
    destroy_if_retired(loop, transaction);
    return;
  }
  transaction->work->preparation = *packet;
  clear_packet(packet);
  transaction->phase = HTTP_TRANSACTION_OPENING;
}

static bool reserve_snapshot(struct loop *loop, struct transaction *transaction,
    size_t capacity)
{
  struct http_body *body = &transaction->snapshot;
  if (capacity > transaction->context.remaining.body_bytes) {
    transaction->failure = CALL_FILE_TOO_LARGE;
    return false;
  }
  if (capacity <= body->capacity) {
    return true;
  }
  if (loop->storage.reserved > HTTP_STORAGE_MAX ||
      capacity > HTTP_STORAGE_MAX - loop->storage.reserved) {
    transaction->failure = CALL_QUOTA;
    return false;
  }
  loop->storage.reserved += capacity;
  unsigned char *data = malloc(capacity);
  if (!data) {
    loop->storage.reserved -= capacity;
    transaction->failure = CALL_NO_MEMORY;
    return false;
  }
  if (body->size) {
    memcpy(data, body->data, body->size);
  }
  free(body->data);
  loop->storage.reserved -= body->capacity;
  body->data = data;
  body->capacity = capacity;
  body->storage = &loop->storage;
  return true;
}

static bool append_snapshot(struct loop *loop, struct transaction *transaction,
    const void *bytes, size_t size)
{
  struct http_body *body = &transaction->snapshot;
  uint64_t limit = transaction->context.remaining.body_bytes;
  if (body->size > limit || size > limit - body->size) {
    transaction->failure = CALL_FILE_TOO_LARGE;
    return false;
  }
  size_t needed = body->size + size;
  if (needed > body->capacity) {
    size_t capacity = body->capacity ? body->capacity : 4096;
    if (capacity > limit) {
      capacity = limit;
    }
    while (capacity < needed) {
      if (capacity > limit / 2) {
        capacity = limit;
        break;
      }
      capacity *= 2;
    }
    if (!reserve_snapshot(loop, transaction, capacity)) {
      return false;
    }
  }
  memcpy(body->data + body->size, bytes, size);
  body->size += size;
  return true;
}

static void legacy_publish(struct loop *loop, struct transaction *transaction, bool redirect)
{
  struct http_response *response = &transaction->response;
  struct provider_open_reply header = {.provider_status = response->status,
    .consumed = response->consumed};
  unsigned char bytes[sizeof(header) + HTTP_URI_MAX];
  handle_t client = HANDLE_INVALID;
  enum call_status status = CALL_OK;
  size_t size;
  if (redirect) {
    header.outcome = PROVIDER_OUTCOME_REDIRECT;
    header.location_size = strlen(response->location);
    header.consumed.body_bytes = http_stream_read_ahead(transaction->engine);
    char destination[HTTP_URI_MAX + 1];
    if (response->invalid_location || !header.location_size) {
      status = CALL_BAD_REQUEST;
    } else {
      status = http_url_resolve(transaction->work->uri, response->location, destination);
      if (status != CALL_OK) {
        status = status == CALL_FILE_TOO_LARGE ? CALL_FILE_TOO_LARGE : CALL_BAD_REQUEST;
      } else if (header.consumed.body_bytes > transaction->context.remaining.body_bytes) {
        status = CALL_FILE_TOO_LARGE;
      }
    }
    memcpy(bytes + sizeof(header), response->location, header.location_size);
    size = sizeof(header) + header.location_size;
    if (status == CALL_OK) {
      status = http_stream_abort(transaction->engine);
      enum call_status closed = http_stream_close(transaction->engine);
      if (closed != CALL_BUSY) {
        transaction->engine = NULL;
      }
      if (status == CALL_OK) {
        status = closed;
      }
      if (status != CALL_OK) {
        transaction->cleanup = status;
      }
    }
  } else {
    status = export(loop, transaction, 0, FACET_SNAPSHOT, &client);
    header.protocol = PROTOCOL_FILE;
    header.representation = PROVIDER_REPRESENTATION_BYTES;
    header.outcome = PROVIDER_OUTCOME_BYTES;
    header.media_type_size = strlen(response->media_type);
    memcpy(bytes + sizeof(header), response->media_type, header.media_type_size);
    size = sizeof(header) + header.media_type_size;
  }
  if (status == CALL_OK) {
    memcpy(bytes, &header, sizeof(header));
    struct endpoint_grant grant = {client, FILE_RIGHT_READ, HANDLE_TRANSPORT_CALL};
    status = reply(loop, &transaction->work->preparation, CALL_OK, bytes, size,
        redirect ? NULL : &grant, redirect ? 0 : 1);
  } else {
    legacy_error(loop, &transaction->work->preparation, status, response->status);
  }
  if (client && handle_close(client)) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  if (status != CALL_OK) {
    withdraw(loop, transaction);
    abort_transaction(loop, transaction, status);
  } else if (redirect) {
    /* Redirect handoff intentionally discards transport, never its body. */
    transaction->phase = HTTP_TRANSACTION_COMPLETE;
  }
}

static void publish_reader(struct loop *loop, struct transaction *transaction)
{
  handle_t reader = HANDLE_INVALID, observation = HANDLE_INVALID;
  enum call_status status = export(loop, transaction, transaction->slot_count - 1,
      FACET_RESPONSE, &reader);
  if (status == CALL_OK) {
    status = export(loop, transaction, transaction->slot_count - 2,
        FACET_OBSERVATION, &observation);
  }
  if (status == CALL_OK) {
    struct endpoint_grant grants[] = {
      {reader, STREAM_RIGHT_READ, HANDLE_TRANSPORT_CALL},
      {observation, HTTP_TRANSACTION_RIGHT_STATUS, HANDLE_TRANSPORT_CALL},
    };
    struct http_commit_reply response = {HTTP_COMMIT_RESPONSE, transaction->response.status};
    status = reply(loop, &transaction->work->commit, CALL_OK, &response, sizeof(response), grants, 2);
  } else {
    reply(loop, &transaction->work->commit, status, NULL, 0, NULL, 0);
  }
  if ((reader && handle_close(reader)) || (observation && handle_close(observation))) {
    loop->fatal = CALL_BAD_HANDLE;
  }
  if (status == CALL_OK) {
    transaction->handed_off = true;
    transaction->phase = HTTP_TRANSACTION_READING;
  } else {
    abort_transaction(loop, transaction, status);
    if (reader) {
      endpoint_withdraw(loop->receiver, object_id(transaction->slots[transaction->slot_count - 1]));
    }
  }
}

static void start_engine(struct loop *loop, struct transaction *transaction)
{
  if (transaction->mutation_failure != CALL_OK) {
    abort_transaction(loop, transaction, transaction->mutation_failure);
    return;
  }
  struct http_request request = {.method = transaction->method,
    .body = transaction->method == HTTP_PREPARE_POST ? transaction->backing : HANDLE_INVALID,
    .body_length = transaction->body_length, .headers = transaction->work->headers_fields,
    .header_count = transaction->work->field_count};
  struct http_response_limits limits = {
    HTTP_HEADERS_MAX, HTTP_FIELDS_MAX, HTTP_INFORMATIONAL_MAX,
  };
  if (transaction->legacy) {
    limits = (struct http_response_limits){transaction->context.remaining.header_bytes,
      transaction->context.remaining.fields, transaction->context.remaining.informational};
  }
  transaction->engine = http_request_start(&loop->client, transaction->work->uri, &request,
      transaction->deadline, &limits, loop->receiver, &loop->next_id, &transaction->response);
  erase(transaction->work->fields, sizeof(transaction->work->fields));
  memset(transaction->work->headers_fields, 0, sizeof(transaction->work->headers_fields));
  if (!transaction->engine) {
    abort_transaction(loop, transaction, response_status(&transaction->response));
  }
}

static void snapshot_request(struct loop *loop, struct transaction *transaction,
    struct endpoint_packet *packet)
{
  struct http_body *body = &transaction->snapshot;
  if (!(packet->rights & FILE_RIGHT_READ)) {
    reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
  } else if (packet->operation == FILE_SIZE && !packet->size) {
    struct file_size_reply response = {body->size};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
  } else if (packet->operation == FILE_INFO && !packet->size) {
    struct file_info_reply response = {.valid = FILE_INFO_SIZE_VALID | FILE_INFO_DOMAIN_VALID,
      .size = body->size, .domain = FILE_DOMAIN_PRIVATE_BYTES};
    reply(loop, packet, CALL_OK, &response, sizeof(response), NULL, 0);
  } else if (packet->operation == FILE_READ && packet->size == sizeof(struct file_read_request)) {
    struct file_read_request request;
    memcpy(&request, packet->data, sizeof(request));
    if (request.capacity > FILE_READ_MAX_BYTES) {
      reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
      return;
    }
    size_t size = request.offset < body->size ? body->size - request.offset : 0;
    if (size > request.capacity) {
      size = request.capacity;
    }
    unsigned char bytes[ENDPOINT_DATA_MAX];
    struct file_read_reply response = {size};
    memcpy(bytes, &response, sizeof(response));
    if (size) {
      memcpy(bytes + sizeof(response), body->data + request.offset, size);
    }
    reply(loop, packet, CALL_OK, bytes, sizeof(response) + size, NULL, 0);
  } else {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
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
    }
  }
  if (transaction->linked && !transaction->unlink_attempted) {
    enum call_status status = unlink_backing(loop, transaction);
    if (status == CALL_OK) {
      return;
    }
    transaction->cleanup = status;
  }
  if (transaction->backing) {
    if (handle_close(transaction->backing)) {
      transaction->cleanup = CALL_BAD_HANDLE;
    }
    transaction->backing = HANDLE_INVALID;
  }
  if (transaction->work && transaction->work->preparation.receipt) {
    if (transaction->legacy) {
      legacy_error(loop, &transaction->work->preparation, transaction->failure, transaction->response.status);
    } else {
      reply(loop, &transaction->work->preparation, transaction->failure, NULL, 0, NULL, 0);
    }
  }
  if (transaction->work && transaction->work->commit.receipt) {
    reply(loop, &transaction->work->commit, transaction->failure, NULL, 0, NULL, 0);
  }
  if (transaction->work && transaction->work->mutation.receipt) {
    reply(loop, &transaction->work->mutation, transaction->failure, NULL, 0, NULL, 0);
  }
  if (transaction->work) {
    deliver_read(loop, transaction, &transaction->work->read);
  }
  if (!has_facet(loop, transaction, FACET_RESPONSE) && transaction->queue) {
    erase(transaction->queue, RESPONSE_BYTES);
    free(transaction->queue);
    transaction->queue = NULL;
    transaction->queued = 0;
  }
  if (transaction->work) {
    erase(transaction->work, sizeof(*transaction->work));
    free(transaction->work);
    transaction->work = NULL;
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
  if (transaction->pending) {
    return false;
  }
  if (transaction->phase == HTTP_TRANSACTION_OPENING && !transaction->engine) {
    start_engine(loop, transaction);
    if (!transaction->engine) {
      settle(loop, transaction);
      return true;
    }
  }
  if (!transaction->engine) {
    return false;
  }
  size_t count = 0;
  size_t capacity = transaction->legacy ? RESPONSE_BYTES :
      RESPONSE_BYTES - transaction->queued;
  if (capacity > HTTP_READ_MAX) {
    capacity = HTTP_READ_MAX;
  }
  if (transaction->legacy && transaction->headers) {
    size_t remaining = transaction->context.remaining.body_bytes - transaction->snapshot.size;
    if (remaining < capacity) {
      capacity = remaining ? remaining : 1;
    }
  }
  unsigned char *output = transaction->queue + (transaction->legacy ? 0 : transaction->queued);
  enum http_step_result step = http_stream_step(transaction->engine,
      output, capacity, &count, &transaction->wait);
  if (step == HTTP_STEP_HEADERS) {
    transaction->headers = true;
    if (transaction->legacy) {
      if (redirect_status(transaction->response.status)) {
        legacy_publish(loop, transaction, true);
      } else if (transaction->response.status != 200 && transaction->response.status != 204) {
        struct http_result result = {.error = HTTP_REJECTED_STATUS,
          .status = transaction->response.status};
        abort_transaction(loop, transaction, http_result_status(&result));
      } else if (transaction->response.length_present &&
          !reserve_snapshot(loop, transaction, transaction->response.length)) {
        abort_transaction(loop, transaction, transaction->failure);
      } else {
        transaction->phase = HTTP_TRANSACTION_READING;
      }
    } else {
      publish_reader(loop, transaction);
    }
  } else if (step == HTTP_STEP_DATA) {
    if (transaction->legacy) {
      if (!append_snapshot(loop, transaction, output, count)) {
        abort_transaction(loop, transaction, transaction->failure);
      }
    } else {
      transaction->queued += count;
      deliver_read(loop, transaction, &transaction->work->read);
    }
  } else if (step == HTTP_STEP_COMPLETE) {
    transaction->phase = HTTP_TRANSACTION_COMPLETE;
    if (transaction->legacy) {
      legacy_publish(loop, transaction, false);
    }
  } else if (step == HTTP_STEP_FAILED) {
    abort_transaction(loop, transaction, response_status(&transaction->response));
  }
  if (!transaction->legacy && transaction->headers && transaction->queued == RESPONSE_BYTES) {
    /* Buffer pressure waits only for clients/deadline, not an already readable
     * socket whose bytes cannot currently be consumed. */
    transaction->wait.stream = HANDLE_INVALID;
    transaction->wait.events = 0;
  }
  settle(loop, transaction);
  return step != HTTP_STEP_WAIT;
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
      abort_transaction(loop, transaction, CALL_ABANDONED);
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
  if (packet->object_id == SERVICE_ID && packet->protocol == PROTOCOL_PROVIDER) {
    loop->published = false;
    loop->fatal = endpoint_retire_ack(loop->receiver, SERVICE_ID);
    return;
  }
  if (packet->object_id < 2 || packet->object_id - 2 >= EXPORT_SLOTS) {
    loop->fatal = CALL_BAD_REQUEST;
    return;
  }
  struct exported *entry = &loop->exports[packet->object_id - 2];
  struct transaction *transaction = entry->owner;
  if (!entry->live || !transaction || packet->protocol != facet_protocol(entry->facet)) {
    loop->fatal = CALL_BAD_REQUEST;
    return;
  }
  enum facet facet = entry->facet;
  enum call_status status = endpoint_retire_ack(loop->receiver, packet->object_id);
  if (status != CALL_OK) {
    loop->fatal = status;
    return;
  }
  entry->live = false;
  if (facet == FACET_CONTROL && !transaction->handed_off &&
      (!transaction->work || !transaction->work->commit.receipt)) {
    abort_transaction(loop, transaction, CALL_ABANDONED);
  } else if (facet == FACET_RESPONSE) {
    transaction->queued = 0;
    if (transaction->queue) {
      erase(transaction->queue, RESPONSE_BYTES);
      free(transaction->queue);
      transaction->queue = NULL;
    }
    abort_transaction(loop, transaction, CALL_ABANDONED);
  } else if (facet == FACET_SNAPSHOT) {
    http_body_release(&transaction->snapshot);
  }
  settle(loop, transaction);
  /* settle may have destroyed a fully retired terminal transaction. */
  if (entry->owner) {
    destroy_if_retired(loop, entry->owner);
  }
}

static void receive_packet(struct loop *loop, struct endpoint_packet *packet)
{
  if (packet->kind == ENDPOINT_MESSAGE_COMPLETION) {
    struct transaction *transaction = loop->active;
    if (transaction && transaction->pending == packet->correlation_id) {
      staging_completion(loop, transaction, packet);
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
  if (packet->object_id == SERVICE_ID && packet->protocol == PROTOCOL_PROVIDER && loop->published) {
    if (packet->operation == PROVIDER_PREPARE) {
      prepare(loop, packet);
    } else if (packet->operation == PROVIDER_OPEN) {
      legacy_open(loop, packet);
    } else {
      reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
    }
    return;
  }
  if (packet->object_id < 2 || packet->object_id - 2 >= EXPORT_SLOTS) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
    return;
  }
  struct exported *entry = &loop->exports[packet->object_id - 2];
  struct transaction *transaction = entry->owner;
  if (!entry->live || !transaction || packet->protocol != facet_protocol(entry->facet)) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
  } else if (entry->facet == FACET_BODY) {
    body_request(loop, transaction, packet);
  } else if (entry->facet == FACET_RESPONSE) {
    response_request(loop, transaction, packet);
  } else if (entry->facet == FACET_SNAPSHOT) {
    snapshot_request(loop, transaction, packet);
  } else if (packet->size) {
    reply(loop, packet, CALL_BAD_REQUEST, NULL, 0, NULL, 0);
  } else if (entry->facet == FACET_OBSERVATION) {
    if (packet->operation == HTTP_TRANSACTION_STATUS && (packet->rights & HTTP_TRANSACTION_RIGHT_STATUS)) {
      publish_status(loop, transaction, packet);
    } else {
      reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    }
  } else if (entry->facet == FACET_CONTROL) {
    if (packet->operation == HTTP_TRANSACTION_COMMIT && (packet->rights & HTTP_TRANSACTION_RIGHT_COMMIT)) {
      commit_request(loop, transaction, packet);
    } else if (packet->operation == HTTP_TRANSACTION_ABORT && (packet->rights & HTTP_TRANSACTION_RIGHT_ABORT)) {
      abort_transaction(loop, transaction, CALL_ABANDONED);
      reply(loop, packet, CALL_OK, NULL, 0, NULL, 0);
    } else {
      reply(loop, packet, CALL_DENIED, NULL, 0, NULL, 0);
    }
  } else {
    reply(loop, packet, CALL_BAD_OPERATION, NULL, 0, NULL, 0);
  }
}

static bool alive(const struct loop *loop)
{
  if (loop->published || loop->active) {
    return true;
  }
  for (size_t i = 0; i < EXPORT_SLOTS; ++i) {
    if (loop->exports[i].owner) {
      return true;
    }
  }
  return false;
}

enum call_status httpfs_serve(handle_t service, handle_t receiver,
    const struct http_client *client, bool custom_trust, handle_t staging)
{
  struct loop loop = {.service = service, .receiver = receiver, .client = *client, .next_id = 1,
    .staging = staging, .published = true, .custom_trust = custom_trust};
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
    bool advanced = progress(&loop);
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
  return loop.fatal;
}
