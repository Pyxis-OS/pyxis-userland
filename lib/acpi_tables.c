#include <acpi_tables.h>
#include <string.h>
#include <syscall.h>

enum call_status acpi_table_info(handle_t directory, const char *name,
    struct acpi_table_info *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct acpi_table_info){0};
  if (!name) {
    return CALL_BAD_REQUEST;
  }
  struct acpi_table_info_request query = {(uintptr_t)name, strlen(name)};
  struct directory_message message = {.header = {PROTOCOL_DIRECTORY, ACPI_TABLES_INFO}};
  memcpy(&message.body, &query, sizeof(query));
  struct acpi_table_info reply;
  struct syscall_result result = syscall_call(directory, &message, sizeof(message),
      &reply, sizeof(reply));
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.reply_size != (result.status == CALL_OK ? sizeof(reply) : 0)) {
    return CALL_BAD_REQUEST;
  }
  if (result.status == CALL_OK) {
    *info = reply;
  }
  return result.status;
}
