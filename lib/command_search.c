#include <command_search.h>
#include <environment.h>
#include <handle.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static enum call_status reply_status(struct syscall_result result, size_t size)
{
  enum call_status status = result.status < CALL_STATUS_COUNT ? result.status : CALL_UNAVAILABLE;
  if ((status == CALL_OK && result.reply_size != size) ||
      (status != CALL_OK && result.reply_size)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return status;
}

enum call_status command_list_validate(handle_t list)
{
  struct handle_info info;
  enum call_status status = handle_query(list, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_COMMAND_LIST || info.transport) {
    return CALL_WRONG_TYPE;
  }
  return (info.rights & COMMAND_LIST_RIGHT_READ) ? CALL_OK : CALL_DENIED;
}

enum call_status command_list_info(handle_t list, struct command_list_info_reply *info)
{
  if (!info) {
    return CALL_BAD_REQUEST;
  }
  *info = (struct command_list_info_reply){0};
  enum call_status status = command_list_validate(list);
  if (status != CALL_OK) {
    return status;
  }
  struct message_header request = {PROTOCOL_COMMAND_LIST, COMMAND_LIST_INFO};
  struct command_list_info_reply reply = {0};
  struct syscall_result result = syscall_call(list, &request, sizeof(request), &reply, sizeof(reply));
  status = reply_status(result, sizeof(reply));
  if (status == CALL_OK) {
    *info = reply;
  }
  return status;
}

enum call_status command_list_get(handle_t list, size_t index,
    struct command_list_entry_reply *entry)
{
  if (!entry) {
    return CALL_BAD_REQUEST;
  }
  *entry = (struct command_list_entry_reply){0};
  enum call_status status = command_list_validate(list);
  if (status != CALL_OK) {
    return status;
  }
  struct command_list_get_message request = {
    .header = {PROTOCOL_COMMAND_LIST, COMMAND_LIST_GET}, .index = index,
  };
  struct command_list_entry_reply reply = {0};
  struct syscall_result result = syscall_call(list, &request, sizeof(request), &reply, sizeof(reply));
  status = reply_status(result, sizeof(reply));
  if (status == CALL_OK && !reply.handle) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status == CALL_OK) {
    *entry = reply;
  }
  return status;
}

enum call_status command_list_create(handle_t environment,
    const struct command_list_create_entry *entries, size_t count, handle_t *list)
{
  if (!list) {
    return CALL_BAD_REQUEST;
  }
  *list = HANDLE_INVALID;
  if (count && !entries) {
    return CALL_BAD_REQUEST;
  }
  struct environment_create_command_list_message request = {
    .header = {PROTOCOL_ENVIRONMENT, ENVIRONMENT_CREATE_COMMAND_LIST},
    .entries_address = (uintptr_t)entries, .count = count,
  };
  struct command_list_create_reply reply = {0};
  struct syscall_result result = syscall_call(environment, &request, sizeof(request), &reply, sizeof(reply));
  enum call_status status = reply_status(result, sizeof(reply));
  if (status == CALL_OK && !reply.handle) {
    status = CALL_OUTCOME_UNKNOWN;
  }
  if (status == CALL_OK) {
    *list = reply.handle;
  }
  return status;
}

enum call_status command_list_publish(handle_t environment, handle_t list, bool exported)
{
  enum call_status status = command_list_validate(list);
  return status == CALL_OK ? environment_set_capability(environment, "commands", list,
      COMMAND_LIST_RIGHT_READ, 0, exported ? ENVIRONMENT_EXPORTED : 0) : status;
}

void command_search_capture_close(struct command_search_capture *capture)
{
  if (!capture) {
    return;
  }
  for (size_t i = 0; i < capture->count; ++i) {
    handle_close(capture->entries[i].handle);
  }
  free(capture->entries);
  if (capture->list) {
    handle_close(capture->list);
  }
  *capture = (struct command_search_capture){0};
}

static enum call_status capture_owned_list(handle_t list, struct command_search_capture *capture)
{
  capture->list = list;
  enum call_status status = command_list_validate(list);
  struct command_list_info_reply info = {0};
  if (status == CALL_OK) {
    status = command_list_info(list, &info);
  }
  if (status == CALL_OK && info.count > SIZE_MAX / sizeof(*capture->entries)) {
    status = CALL_LIMIT;
  }
  if (status == CALL_OK && info.count) {
    capture->entries = calloc(info.count, sizeof(*capture->entries));
    if (!capture->entries) {
      status = CALL_NO_MEMORY;
    }
  }
  capture->capacity = info.capacity;
  while (status == CALL_OK && capture->count < info.count) {
    status = command_list_get(list, capture->count, &capture->entries[capture->count]);
    if (status == CALL_OK) {
      ++capture->count;
    }
  }
  if (status != CALL_OK) {
    command_search_capture_close(capture);
  }
  return status;
}

enum call_status command_search_capture_init(handle_t environment,
    struct command_search_capture *capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  *capture = (struct command_search_capture){0};
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(environment, "commands", &entry, NULL, 0);
  if (status == CALL_BUFFER_TOO_SMALL || (status == CALL_OK && entry.kind != ENVIRONMENT_CAP)) {
    return CALL_WRONG_TYPE;
  }
  return status == CALL_OK ? capture_owned_list(entry.handle, capture) : status;
}

enum call_status command_search_capture_list(handle_t list,
    struct command_search_capture *capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  *capture = (struct command_search_capture){0};
  handle_t owned;
  enum call_status status = handle_copy_restricted(list, COMMAND_LIST_RIGHT_READ, 0, &owned);
  return status == CALL_OK ? capture_owned_list(owned, capture) : status;
}

enum call_status command_search_stage(const struct command_search_capture *capture,
    struct launch_grant *grants, size_t grant_capacity, size_t *grant_count,
    struct launch_environment_entry *entries, size_t entry_capacity, size_t *entry_count)
{
  if (!capture || !capture->list || !grant_count || !entry_count ||
      *grant_count > grant_capacity || *entry_count > entry_capacity ||
      (grant_capacity && !grants) || (entry_capacity && !entries)) {
    return CALL_BAD_REQUEST;
  }
  if (*grant_count == grant_capacity || *entry_count == entry_capacity) {
    return CALL_LIMIT;
  }
  for (size_t i = 0; i < *entry_count; ++i) {
    if (!strcmp((const char *)(uintptr_t)entries[i].name, "commands")) {
      return CALL_ALREADY_EXISTS;
    }
  }
  entries[(*entry_count)++] = (struct launch_environment_entry){
    .name = (uintptr_t)"commands", .kind = ENVIRONMENT_CAP,
    .flags = ENVIRONMENT_EXPORTED, .value = *grant_count,
  };
  grants[(*grant_count)++] = (struct launch_grant){capture->list, COMMAND_LIST_RIGHT_READ, 0};
  return CALL_OK;
}
