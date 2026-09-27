#include <abi/file.h>
#include <file.h>
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

static enum call_status call_status(struct syscall_result result, size_t reply_size)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}

static enum call_status mutation_status(struct syscall_result result, size_t reply_size)
{
  /* Once submitted, an untrustworthy reply cannot establish no side effects. */
  if (result.status >= CALL_STATUS_COUNT ||
      result.reply_size != (result.status == CALL_OK ? reply_size : 0)) {
    return CALL_OUTCOME_UNKNOWN;
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
  enum call_status status = call_status(syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
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
  struct syscall_result result = syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply.body) + limit);
  if (result.status >= CALL_STATUS_COUNT) {
    *read = 0;
    return CALL_UNAVAILABLE;
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
  enum call_status status = mutation_status(syscall_call(file, &message,
      offsetof(struct file_write_message, bytes) + limit, &reply, sizeof(reply)),
      sizeof(reply));
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
  return mutation_status(syscall_call(file, &message, sizeof(message), NULL, 0), 0);
}

enum call_status file_sync(handle_t file)
{
  struct message_header message = {PROTOCOL_FILE, FILE_SYNC};
  return mutation_status(syscall_call(file, &message, sizeof(message), NULL, 0), 0);
}
