#include <space.h>
#include <string.h>
#include <syscall.h>

enum call_status space_set_title(handle_t space, const char *title)
{
  if (!title) {
    return CALL_BAD_REQUEST;
  }
  size_t length = strnlen(title, SPACE_TITLE_MAX + 1);
  if (!length || length > SPACE_TITLE_MAX) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < length; ++i) {
    if ((unsigned char)title[i] < 0x20 || (unsigned char)title[i] > 0x7e) {
      return CALL_BAD_REQUEST;
    }
  }
  struct space_title_request request = {
    .header = {PROTOCOL_SPACE, SPACE_SET_TITLE},
    .title = (uintptr_t)title,
    .length = length,
  };
  struct syscall_result result = syscall_call(space, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size) {
    return CALL_BAD_REQUEST;
  }
  return result.status;
}
