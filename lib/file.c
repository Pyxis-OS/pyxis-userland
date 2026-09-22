#include <abi/file.h>
#include <file.h>
#include <syscall.h>

int file_size(handle_t file, uint64_t *size)
{
  if (!size) {
    return -1;
  }
  *size = 0;

  struct file_message message = {.header = {PROTOCOL_FILE, FILE_SIZE}};
  struct file_size_reply reply;
  struct syscall_result result = syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply)) {
    return -1;
  }
  *size = reply.size;
  return 0;
}

int file_read(handle_t file, uint64_t offset, void *bytes, size_t capacity,
              size_t *read)
{
  if (!read) {
    return -1;
  }
  *read = 0;

  struct file_message message = {
    .header = {PROTOCOL_FILE, FILE_READ},
    .body.read = {
      .offset = offset,
      .address = (uintptr_t)bytes,
      .capacity = capacity,
    },
  };
  struct file_read_reply reply;
  struct syscall_result result = syscall_call(file, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply) ||
      reply.read > capacity || reply.read > UINT64_MAX - offset) {
    return -1;
  }
  *read = reply.read;
  return 0;
}
