#include <forwarder.h>
#include <syscall.h>

static enum call_status checked_status(struct syscall_result result)
{
  return result.status < CALL_STATUS_COUNT ? result.status : CALL_UNAVAILABLE;
}

enum call_status forwarder_create(const struct forwarder_create *request,
    struct forwarder_reply *reply)
{
  if (!request || !reply) {
    return CALL_BAD_REQUEST;
  }
  struct forwarder_create captured = *request;
  *reply = (struct forwarder_reply){0};
  struct forwarder_reply created = {0};
  struct syscall_result result = syscall_forwarder_create(&captured, &created);
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
  }
  if (result.reply_size != sizeof(created) || created.holder == HANDLE_INVALID ||
      created.controller == HANDLE_INVALID || created.holder == created.controller) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *reply = created;
  return CALL_OK;
}

enum call_status forwarder_revocable(handle_t target, uint64_t rights,
    uint64_t transport, uint64_t flags, struct forwarder_reply *reply)
{
  struct forwarder_create request = {target, FORWARDER_REVOCABLE, rights, transport, flags};
  return forwarder_create(&request, reply);
}

enum call_status forwarder_slot(handle_t target, uint64_t rights,
    uint64_t transport, uint64_t flags, struct forwarder_reply *reply)
{
  struct forwarder_create request = {target, FORWARDER_SLOT, rights, transport, flags};
  return forwarder_create(&request, reply);
}

enum call_status forwarder_cut(handle_t controller)
{
  struct message_header request = {PROTOCOL_FORWARDER, FORWARDER_CUT};
  struct syscall_result result = syscall_call(controller, &request, sizeof(request), NULL, 0);
  return result.reply_size == 0 ? checked_status(result) : CALL_OUTCOME_UNKNOWN;
}

enum call_status forwarder_retarget(handle_t controller, handle_t target)
{
  struct forwarder_retarget request = {
    .header = {PROTOCOL_FORWARDER, FORWARDER_RETARGET}, .target = target,
  };
  struct syscall_result result = syscall_call(controller, &request, sizeof(request), NULL, 0);
  return result.reply_size == 0 ? checked_status(result) : CALL_OUTCOME_UNKNOWN;
}

enum call_status forwarder_match(handle_t controller, handle_t holder)
{
  struct forwarder_match request = {
    .header = {PROTOCOL_FORWARDER, FORWARDER_MATCH}, .holder = holder,
  };
  struct syscall_result result = syscall_call(controller, &request, sizeof(request), NULL, 0);
  return result.reply_size == 0 ? checked_status(result) : CALL_OUTCOME_UNKNOWN;
}
