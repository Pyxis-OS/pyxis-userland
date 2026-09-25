#include <random.h>
#include <string.h>
#include <syscall.h>

enum call_status random_read(handle_t random, void *bytes, size_t length, uint64_t deadline_ns)
{
  if (length > RANDOM_MAX_BYTES) {
    return CALL_LIMIT;
  }
  if (length && !bytes) {
    return CALL_BAD_BUFFER;
  }
  struct random_read_request request = {
    .header = {PROTOCOL_RANDOM, RANDOM_READ}, .length = length, .deadline_ns = deadline_ns,
  };
  uint8_t response[RANDOM_MAX_BYTES];
  struct syscall_result result = syscall_call(random, &request, sizeof(request), response, length);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? length : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK && length) {
    memcpy(bytes, response, length);
  }
  return result.status;
}
