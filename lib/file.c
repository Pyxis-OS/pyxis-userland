#include <abi/file.h>
#include <file.h>
#include <syscall.h>

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

  struct file_message message = {.header = {PROTOCOL_FILE, FILE_SIZE}};
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

  struct file_message message = {
    .header = {PROTOCOL_FILE, FILE_READ},
    .body.read = {
      .offset = offset,
      .address = (uintptr_t)bytes,
      .capacity = capacity,
    },
  };
  struct file_read_reply reply;
  enum call_status status = call_status(syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  *read = 0;
  if (status != CALL_OK) {
    return status;
  }
  if (reply.read > capacity || reply.read > UINT64_MAX - offset) {
    return CALL_BAD_REQUEST;
  }
  *read = reply.read;
  return CALL_OK;
}

enum call_status file_write(handle_t file, uint64_t offset, const void *bytes,
                             size_t size, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
  }
  struct file_message message = {
    .header = {PROTOCOL_FILE, FILE_WRITE},
    .body.write = {offset, (uintptr_t)bytes, size},
  };
  struct file_write_reply reply;
  enum call_status status = mutation_status(syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply)), sizeof(reply));
  /* Consume the source first, even when written aliases it. */
  *written = 0;
  if (status != CALL_OK) {
    return status;
  }
  if (reply.written > size || (size && !reply.written) ||
      reply.written > UINT64_MAX - offset) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *written = reply.written;
  return CALL_OK;
}

enum call_status file_resize(handle_t file, uint64_t size)
{
  struct file_message message = {
    .header = {PROTOCOL_FILE, FILE_RESIZE},
    .body.resize.size = size,
  };
  return mutation_status(syscall_call(file, &message, sizeof(message), NULL, 0), 0);
}

enum call_status file_sync(handle_t file)
{
  struct file_message message = {.header = {PROTOCOL_FILE, FILE_SYNC}};
  return mutation_status(syscall_call(file, &message, sizeof(message), NULL, 0), 0);
}
