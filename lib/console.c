#include <abi/console.h>
#include <console.h>
#include <syscall.h>

int console_write(handle_t output, const void *bytes, size_t size, size_t *written)
{
  if (!written) {
    return -1;
  }
  *written = 0;
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_WRITE},
    .body.write = {
      .address = (uintptr_t)bytes,
      .length = size,
    },
  };
  struct console_write_reply reply;
  struct syscall_result result = syscall_call(output, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply) ||
      reply.written > size || (size && !reply.written)) {
    return -1;
  }
  *written = reply.written;
  return 0;
}

int console_write_all(handle_t output, const void *bytes, size_t size)
{
  const char *cursor = bytes;
  while (size) {
    size_t written;
    if (console_write(output, cursor, size, &written) != 0) {
      return -1;
    }
    cursor += written;
    size -= written;
  }
  return 0;
}

int console_print(handle_t output, const char *text)
{
  size_t size = 0;
  while (text[size]) {
    ++size;
  }
  return console_write_all(output, text, size);
}

enum call_status console_read(handle_t input, void *bytes, size_t capacity, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_READ},
    .body.read = {.address = (uintptr_t)bytes, .capacity = capacity},
  };
  struct console_read_reply reply;
  struct syscall_result result = syscall_call(input, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.read > capacity ||
      (capacity && !reply.read)) {
    return CALL_BAD_REQUEST;
  }
  *read = reply.read;
  return CALL_OK;
}

enum call_status console_size(handle_t console, size_t *columns, size_t *rows)
{
  if (!columns || !rows) {
    return CALL_BAD_REQUEST;
  }
  *columns = 0;
  *rows = 0;
  struct console_message message = {.header = {PROTOCOL_CONSOLE, CONSOLE_SIZE}};
  struct console_size_reply reply;
  struct syscall_result result = syscall_call(console, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || !reply.columns || !reply.rows) {
    return CALL_BAD_REQUEST;
  }
  *columns = reply.columns;
  *rows = reply.rows;
  return CALL_OK;
}
