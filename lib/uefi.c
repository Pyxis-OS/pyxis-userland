#include <uefi.h>
#include <syscall.h>

enum call_status uefi_boot_register(handle_t uefi,
    const struct uefi_boot_register_request *request)
{
  if (!request) return CALL_BAD_REQUEST;
  struct {
    struct message_header header;
    struct uefi_boot_register_request body;
  } message = {{PROTOCOL_UEFI, UEFI_BOOT_REGISTER}, *request};
  struct syscall_result result = syscall_call(uefi, &message, sizeof(message), NULL, 0);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size) return CALL_OUTCOME_UNKNOWN;
  return result.status;
}
