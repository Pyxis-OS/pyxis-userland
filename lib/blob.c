#include <abi/blob.h>
#include <blob.h>
#include <syscall.h>

int blob_size(handle_t content, uint64_t *size)
{
  if (!size) {
    return -1;
  }
  *size = 0;

  struct blob_size_reply reply;
  struct syscall_result result = syscall_call(content, BLOB_SIZE, NULL, 0,
      &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply)) {
    return -1;
  }
  *size = reply.size;
  return 0;
}

int blob_read(handle_t content, uint64_t offset, void *bytes, size_t capacity,
              size_t *read)
{
  if (!read) {
    return -1;
  }
  *read = 0;

  struct blob_read_request request = {
    .offset = offset,
    .address = (uintptr_t)bytes,
    .capacity = capacity,
  };
  struct blob_read_reply reply;
  struct syscall_result result = syscall_call(content, BLOB_READ,
      &request, sizeof(request), &reply, sizeof(reply));
  if (result.status != CALL_OK || result.reply_size != sizeof(reply) ||
      reply.read > capacity || reply.read > UINT64_MAX - offset) {
    return -1;
  }
  *read = reply.read;
  return 0;
}
