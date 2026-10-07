#include <abi/console.h>
#include <console.h>
#include <syscall.h>

enum call_status console_write(handle_t output, const void *bytes, size_t size, size_t *written)
{
  if (!written) {
    return CALL_BAD_REQUEST;
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
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.written > size ||
      (size && !reply.written)) {
    return CALL_BAD_REQUEST;
  }
  *written = reply.written;
  return CALL_OK;
}

enum call_status console_write_all(handle_t output, const void *bytes, size_t size)
{
  const char *cursor = bytes;
  while (size) {
    size_t written;
    enum call_status status = console_write(output, cursor, size, &written);
    if (status != CALL_OK) {
      return status;
    }
    cursor += written;
    size -= written;
  }
  return CALL_OK;
}

enum call_status console_print(handle_t output, const char *text)
{
  size_t size = 0;
  while (text[size]) {
    ++size;
  }
  return console_write_all(output, text, size);
}

static enum call_status read_console(handle_t input, void *bytes, size_t capacity,
                                    uint64_t timeout_ms, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_READ},
    .body.read = {.address = (uintptr_t)bytes, .capacity = capacity, .timeout_ms = timeout_ms},
  };
  struct console_read_reply reply;
  struct syscall_result result = syscall_call(input, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.read > capacity) {
    return CALL_BAD_REQUEST;
  }
  *read = reply.read;
  return CALL_OK;
}

enum call_status console_read(handle_t input, void *bytes, size_t capacity, size_t *read)
{
  return read_console(input, bytes, capacity, CONSOLE_WAIT_FOREVER, read);
}

enum call_status console_read_timeout(handle_t input, void *bytes, size_t capacity,
                                     uint32_t timeout_ms, size_t *read)
{
  return read_console(input, bytes, capacity, timeout_ms, read);
}

enum call_status console_size(handle_t console, struct console_size_reply *size)
{
  if (!size) {
    return CALL_BAD_REQUEST;
  }
  *size = (struct console_size_reply){0};
  struct console_message message = {.header = {PROTOCOL_CONSOLE, CONSOLE_SIZE}};
  struct console_size_reply reply;
  struct syscall_result result = syscall_call(console, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || !reply.columns || !reply.rows ||
      !reply.generation) {
    return CALL_BAD_REQUEST;
  }
  *size = reply;
  return CALL_OK;
}

enum call_status console_fresh_line(handle_t output)
{
  struct console_message message = {.header = {PROTOCOL_CONSOLE, CONSOLE_FRESH_LINE}};
  struct syscall_result result = syscall_call(output, &message, sizeof(message), NULL, 0);
  return result.status < CALL_STATUS_COUNT && !result.reply_size ?
         (enum call_status)result.status : CALL_BAD_REQUEST;
}

enum call_status console_set_tab_width(handle_t output, size_t columns)
{
  struct console_message message = {
    .header = {PROTOCOL_CONSOLE, CONSOLE_SET_TAB_WIDTH},
    .body.tab_width = {.columns = columns},
  };
  struct syscall_result result = syscall_call(output, &message, sizeof(message), NULL, 0);
  return result.status < CALL_STATUS_COUNT && !result.reply_size ?
         (enum call_status)result.status : CALL_BAD_REQUEST;
}

static enum call_status console_handle(handle_t input, uint64_t operation, handle_t *handle)
{
  if (!handle) {
    return CALL_BAD_REQUEST;
  }
  *handle = HANDLE_INVALID;
  struct console_message message = {.header = {PROTOCOL_CONSOLE, operation}};
  struct console_handle_reply reply;
  struct syscall_result result = syscall_call(input, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK) {
    return result.status < CALL_STATUS_COUNT && !result.reply_size ?
           (enum call_status)result.status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.handle == HANDLE_INVALID) {
    return CALL_BAD_REQUEST;
  }
  *handle = reply.handle;
  return CALL_OK;
}

enum call_status console_arm_interrupt(handle_t input, handle_t *armed)
{
  return console_handle(input, CONSOLE_ARM_INTERRUPT, armed);
}

enum call_status console_passthrough(handle_t input, handle_t *passthrough)
{
  return console_handle(input, CONSOLE_PASSTHROUGH, passthrough);
}
