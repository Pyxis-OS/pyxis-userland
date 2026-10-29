#include <process_view.h>
#include <syscall.h>

static enum call_status accept_reply(struct syscall_result result, void *buffer,
    struct process_list *list)
{
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  if (result.status != CALL_OK) {
    return result.status;
  }
  const struct process_list_header *header = buffer;
  if (result.reply_size < sizeof(*header)) {
    return CALL_BAD_REQUEST;
  }
  size_t record_bytes = result.reply_size - sizeof(*header);
  if (header->version != PROCESS_LIST_VERSION ||
      header->record_size != sizeof(struct process_list_record) ||
      record_bytes % sizeof(struct process_list_record) ||
      record_bytes / sizeof(struct process_list_record) != header->returned ||
      header->returned > header->total) {
    return CALL_BAD_REQUEST;
  }
  list->header = header;
  list->records = (const struct process_list_record *)(header + 1);
  return CALL_OK;
}

enum call_status process_list_own_space(void *buffer, size_t capacity, struct process_list *list)
{
  *list = (struct process_list){0};
  return accept_reply(syscall_space_processes(buffer, capacity), buffer, list);
}

enum call_status process_list_all(handle_t view, void *buffer, size_t capacity,
    struct process_list *list)
{
  *list = (struct process_list){0};
  struct message_header request = {PROTOCOL_PROCESS_VIEW, PROCESS_VIEW_LIST};
  return accept_reply(syscall_call(view, &request, sizeof(request), buffer, capacity),
      buffer, list);
}
