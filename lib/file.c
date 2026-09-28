#include <abi/file.h>
#include <endpoint.h>
#include <file.h>
#include <handle.h>
#include <stdbool.h>
#include <string.h>
#include <syscall.h>

struct file_read_message {
  struct message_header header;
  struct file_read_request body;
};

struct file_write_message {
  struct message_header header;
  struct file_write_request body;
  unsigned char bytes[FILE_WRITE_MAX_BYTES];
};

struct file_read_response {
  struct file_read_reply body;
  unsigned char bytes[FILE_READ_MAX_BYTES];
};

struct file_resize_message {
  struct message_header header;
  struct file_resize_request body;
};

_Static_assert(offsetof(struct file_write_message, bytes) ==
    sizeof(struct message_header) + sizeof(struct file_write_request),
    "file write bytes follow the request");
_Static_assert(offsetof(struct file_read_response, bytes) ==
    sizeof(struct file_read_reply), "file read bytes follow the reply");

/* The capability selects native dispatch or userspace transport. Protocol and
 * authority are immutable properties of the held grant, including after closure. */
static struct syscall_result file_call(handle_t file, const void *message,
    size_t message_size, void *reply, size_t capacity, bool mutation)
{
  const struct message_header *header = message;
  struct handle_info info;
  enum call_status status = handle_query(file, &info);
  if (status != CALL_OK) {
    return (struct syscall_result){status, 0};
  }
  if (info.protocol != PROTOCOL_FILE ||
      (info.kind != HANDLE_KIND_NATIVE && info.kind != HANDLE_KIND_EXPORTED)) {
    return (struct syscall_result){CALL_WRONG_TYPE, 0};
  }
  uint64_t required = header->operation == FILE_READ ? FILE_RIGHT_READ : FILE_RIGHT_WRITE;
  if (header->operation == FILE_SIZE ? !(info.rights & FILE_RIGHTS) :
      (info.rights & required) != required) {
    return (struct syscall_result){CALL_DENIED, 0};
  }
  if (info.kind == HANDLE_KIND_NATIVE) {
    if (info.transport != 0) {
      return (struct syscall_result){CALL_BAD_REQUEST, 0};
    }
    return syscall_call(file, message, message_size, reply, capacity);
  }
  if ((info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL) {
    return (struct syscall_result){CALL_DENIED, 0};
  }
  struct endpoint_packet packet;
  status = endpoint_invoke(file, PROTOCOL_FILE, header->operation,
      (const unsigned char *)message + sizeof(*header), message_size - sizeof(*header),
      NULL, 0, 0, &packet);
  if (status != CALL_OK) {
    if (mutation && packet.delivery == ENDPOINT_DELIVERED) {
      status = CALL_OUTCOME_UNKNOWN;
    } else if (!mutation && status == CALL_OUTCOME_UNKNOWN) {
      status = CALL_BAD_REQUEST;
    }
    return (struct syscall_result){status, 0};
  }
  bool malformed = packet.grant_count != 0 || packet.result >= CALL_STATUS_COUNT ||
      packet.size > capacity || (packet.result != CALL_OK && packet.size != 0);
  for (size_t i = 0; i < packet.grant_count; ++i) {
    handle_close(packet.grants[i].handle);
  }
  if (malformed) {
    return (struct syscall_result){mutation ? CALL_OUTCOME_UNKNOWN : CALL_BAD_REQUEST, 0};
  }
  if (packet.size) {
    memcpy(reply, packet.data, packet.size);
  }
  return (struct syscall_result){packet.result, packet.size};
}

static enum call_status reply_status(struct syscall_result result, size_t reply_size,
    bool mutation)
{
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return mutation ? CALL_OUTCOME_UNKNOWN : CALL_BAD_REQUEST;
  }
  return result.status;
}

enum call_status file_size(handle_t file, uint64_t *size)
{
  if (!size) {
    return CALL_BAD_REQUEST;
  }
  *size = 0;

  struct message_header message = {PROTOCOL_FILE, FILE_SIZE};
  struct file_size_reply reply;
  enum call_status status = reply_status(file_call(file, &message, sizeof(message),
      &reply, sizeof(reply), false), sizeof(reply), false);
  if (status != CALL_OK) {
    return status;
  }
  *size = reply.size;
  return CALL_OK;
}

enum call_status file_read(handle_t file, uint64_t offset, void *bytes, size_t capacity,
                            size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  if (capacity && !bytes) {
    *read = 0;
    return CALL_BAD_BUFFER;
  }

  size_t limit = capacity < FILE_READ_MAX_BYTES ? capacity : FILE_READ_MAX_BYTES;
  struct file_read_message message = {
    .header = {PROTOCOL_FILE, FILE_READ},
    .body = {offset, limit},
  };
  struct file_read_response reply;
  struct syscall_result result = file_call(file, &message, sizeof(message),
      &reply, sizeof(reply.body) + limit, false);
  if (result.status >= CALL_STATUS_COUNT) {
    *read = 0;
    return CALL_BAD_REQUEST;
  }
  if (result.status != CALL_OK) {
    *read = 0;
    return result.reply_size ? CALL_BAD_REQUEST : (enum call_status)result.status;
  }
  if (result.reply_size < sizeof(reply.body) ||
      result.reply_size > sizeof(reply.body) + limit ||
      reply.body.read != result.reply_size - sizeof(reply.body) ||
      reply.body.read > UINT64_MAX - offset) {
    *read = 0;
    return CALL_BAD_REQUEST;
  }

  size_t count = (size_t)reply.body.read;
  if (count) {
    memcpy(bytes, reply.bytes, count);
  }
  *read = count;
  return CALL_OK;
}

enum call_status file_write(handle_t file, uint64_t offset, const void *bytes,
                             size_t size, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
  }
  if (size && !bytes) {
    *written = 0;
    return CALL_BAD_BUFFER;
  }
  if (size > UINT64_MAX - offset) {
    *written = 0;
    return CALL_BAD_REQUEST;
  }

  size_t limit = size < FILE_WRITE_MAX_BYTES ? size : FILE_WRITE_MAX_BYTES;
  struct file_write_message message = {
    .header = {PROTOCOL_FILE, FILE_WRITE},
    .body = {offset, limit},
  };
  if (limit) {
    memcpy(message.bytes, bytes, limit);
  }
  struct file_write_reply reply;
  enum call_status status = reply_status(file_call(file, &message,
      offsetof(struct file_write_message, bytes) + limit, &reply, sizeof(reply), true),
      sizeof(reply), true);
  /* Consume the source first, even when written aliases it. */
  *written = 0;
  if (status != CALL_OK) {
    return status;
  }
  if (reply.written > limit || (limit && !reply.written)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *written = (size_t)reply.written;
  return CALL_OK;
}

enum call_status file_resize(handle_t file, uint64_t size)
{
  struct file_resize_message message = {
    .header = {PROTOCOL_FILE, FILE_RESIZE},
    .body = {size},
  };
  return reply_status(file_call(file, &message, sizeof(message), NULL, 0, true), 0, true);
}

enum call_status file_sync(handle_t file)
{
  struct message_header message = {PROTOCOL_FILE, FILE_SYNC};
  return reply_status(file_call(file, &message, sizeof(message), NULL, 0, true), 0, true);
}
