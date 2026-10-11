#include <input_policy.h>
#include <syscall.h>

enum call_status input_policy_install_once(handle_t input_policy,
    enum input_scroll_direction mouse, enum input_scroll_direction touchpad)
{
  if ((mouse != INPUT_SCROLL_NATURAL && mouse != INPUT_SCROLL_TRADITIONAL) ||
      (touchpad != INPUT_SCROLL_NATURAL && touchpad != INPUT_SCROLL_TRADITIONAL)) {
    return CALL_BAD_REQUEST;
  }
  struct input_policy_request request = {
    .header = {PROTOCOL_INPUT_POLICY, INPUT_POLICY_INSTALL_ONCE},
    .mouse_scroll_direction = mouse,
    .touchpad_scroll_direction = touchpad,
  };
  struct syscall_result result = syscall_call(input_policy, &request, sizeof(request), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size ? CALL_BAD_REQUEST : result.status;
}
