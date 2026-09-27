#include <endpoint.h>
#include <syscall.h>
#include <stdbool.h>

static enum call_status checked_status(struct syscall_result result)
{
  return result.status < CALL_STATUS_COUNT ? result.status : CALL_UNAVAILABLE;
}

static bool packet_valid(const struct endpoint_packet *packet, size_t size,
    bool received, uint64_t deadline_ns, uint64_t protocol, uint64_t operation)
{
  if (size < ENDPOINT_PACKET_HEADER_SIZE ||
      packet->size > ENDPOINT_DATA_MAX ||
      size != ENDPOINT_PACKET_HEADER_SIZE + packet->size ||
      packet->grant_count > ENDPOINT_GRANTS_MAX) {
    return false;
  }
  if (received) {
    if (packet->kind == ENDPOINT_MESSAGE_RETIRE) {
      return packet->receipt == HANDLE_INVALID &&
          packet->delivery == ENDPOINT_NOT_DELIVERED &&
          packet->object_id != 0 && packet->protocol != 0 &&
          packet->operation == 0 && packet->rights == 0 &&
          packet->reason == 0 && packet->deadline_ns == 0 &&
          packet->result == 0 && packet->size == 0 && packet->grant_count == 0;
    }
    if (packet->receipt == HANDLE_INVALID ||
        packet->delivery != ENDPOINT_DELIVERED || packet->result != 0 ||
        (packet->kind != ENDPOINT_MESSAGE_CALL &&
        packet->kind != ENDPOINT_MESSAGE_SEND &&
        packet->kind != ENDPOINT_MESSAGE_CANCEL) ||
        (packet->kind == ENDPOINT_MESSAGE_SEND && packet->deadline_ns != 0) ||
        (packet->kind == ENDPOINT_MESSAGE_CANCEL &&
        ((packet->reason != CALL_TIMED_OUT && packet->reason != CALL_ENDPOINT_CLOSED) ||
        packet->size != 0 || packet->grant_count != 0))) {
      return false;
    }
  } else if (packet->receipt != HANDLE_INVALID ||
      packet->delivery != ENDPOINT_DELIVERED ||
      packet->kind != ENDPOINT_MESSAGE_CALL ||
      packet->deadline_ns != deadline_ns ||
      packet->protocol != protocol || packet->operation != operation ||
      packet->reason != 0) {
    return false;
  }
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (packet->grants[i].handle == HANDLE_INVALID) {
      return false;
    }
  }
  return true;
}

static enum call_status make_message(struct endpoint_message *message,
    uint64_t outer_protocol, uint64_t endpoint_operation,
    uint64_t protocol, uint64_t operation,
    const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count)
{
  if (size > ENDPOINT_DATA_MAX || (size && !bytes) ||
      grant_count > ENDPOINT_GRANTS_MAX || (grant_count && !grants)) {
    return CALL_BAD_REQUEST;
  }
  *message = (struct endpoint_message){
    .header = {outer_protocol, endpoint_operation},
    .buffer = (uintptr_t)bytes,
    .size = size,
    .grant_count = grant_count,
    .protocol = protocol,
    .operation = operation,
  };
  for (size_t i = 0; i < grant_count; ++i) {
    message->grants[i] = grants[i];
  }
  return CALL_OK;
}

enum call_status endpoint_create(handle_t service, struct endpoint_create_reply *endpoints)
{
  if (!endpoints) {
    return CALL_BAD_REQUEST;
  }
  struct message_header request = {PROTOCOL_ENDPOINT_SERVICE, ENDPOINT_CREATE};
  struct endpoint_create_reply reply = {0};
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
      &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status == CALL_OK) {
    if (result.reply_size != sizeof(reply) ||
        reply.receiver == HANDLE_INVALID || reply.caller == HANDLE_INVALID ||
        reply.receiver == reply.caller) {
      status = CALL_OUTCOME_UNKNOWN;
    } else {
      *endpoints = reply;
      return CALL_OK;
    }
  }
  *endpoints = (struct endpoint_create_reply){0};
  return result.reply_size == 0 || status == CALL_OUTCOME_UNKNOWN ? status : CALL_BAD_REQUEST;
}

enum call_status endpoint_export(handle_t service, handle_t receiver,
    uint64_t object_id, uint64_t protocol, uint64_t rights, uint64_t transport,
    handle_t *client)
{
  if (!client) {
    return CALL_BAD_REQUEST;
  }
  *client = HANDLE_INVALID;
  struct endpoint_export_message request = {
    .header = {PROTOCOL_ENDPOINT_SERVICE, ENDPOINT_EXPORT},
    .receiver = receiver,
    .object_id = object_id,
    .protocol = protocol,
    .rights = rights,
    .transport = transport,
  };
  struct endpoint_export_reply reply = {0};
  struct syscall_result result = syscall_call(service, &request, sizeof(request),
      &reply, sizeof(reply));
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    return result.reply_size == 0 ? status : CALL_BAD_REQUEST;
  }
  if (result.reply_size != sizeof(reply) || reply.client == HANDLE_INVALID) {
    return CALL_OUTCOME_UNKNOWN;
  }
  *client = reply.client;
  return CALL_OK;
}

static enum call_status control(handle_t receiver, uint64_t operation,
    uint64_t object_id)
{
  struct endpoint_control_message message = {
    .header = {PROTOCOL_ENDPOINT_RECEIVER, operation},
    .object_id = object_id,
  };
  struct syscall_result result = syscall_call(receiver, &message, sizeof(message),
      NULL, 0);
  enum call_status status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}

enum call_status endpoint_withdraw(handle_t receiver, uint64_t object_id)
{
  return control(receiver, ENDPOINT_WITHDRAW, object_id);
}

enum call_status endpoint_retire_ack(handle_t receiver, uint64_t object_id)
{
  return control(receiver, ENDPOINT_RETIRE_ACK, object_id);
}

enum call_status endpoint_invoke(handle_t caller, uint64_t protocol,
    uint64_t operation, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, uint64_t deadline_ns,
    struct endpoint_packet *reply)
{
  if (!reply) {
    return CALL_BAD_REQUEST;
  }
  struct endpoint_message message;
  enum call_status status = make_message(&message, PROTOCOL_ENDPOINT, ENDPOINT_CALL,
      protocol, operation, bytes, size, grants, grant_count);
  if (status != CALL_OK) {
    *reply = (struct endpoint_packet){0};
    return status;
  }
  message.deadline_ns = deadline_ns;
  struct endpoint_packet packet = {0};
  struct syscall_result result = syscall_call(caller, &message, sizeof(message),
      &packet, sizeof(packet));
  status = checked_status(result);
  if (status == CALL_OK) {
    if (!packet_valid(&packet, result.reply_size, false, deadline_ns,
        protocol, operation)) {
      *reply = (struct endpoint_packet){0};
      return CALL_OUTCOME_UNKNOWN;
    }
    *reply = packet;
    return CALL_OK;
  }
  *reply = (struct endpoint_packet){0};
  if (result.reply_size == 0) {
    return status;
  }
  /* Descriptor validation can fail before the kernel trusts its deadline. */
  bool unread_deadline = packet.deadline_ns == 0 &&
      packet.delivery == ENDPOINT_NOT_DELIVERED &&
      (status == CALL_BAD_REQUEST || status == CALL_BAD_BUFFER);
  if (result.reply_size != ENDPOINT_PACKET_HEADER_SIZE ||
      packet.delivery > ENDPOINT_DELIVERED || packet.receipt != HANDLE_INVALID ||
      packet.kind != ENDPOINT_MESSAGE_CALL || packet.grant_count != 0 || packet.size != 0 ||
      packet.result != 0 || (packet.deadline_ns != deadline_ns && !unread_deadline)) {
    return CALL_OUTCOME_UNKNOWN;
  }
  reply->delivery = packet.delivery;
  reply->deadline_ns = packet.deadline_ns;
  return status;
}

enum call_status endpoint_request(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count, uint64_t deadline_ns,
    struct endpoint_packet *reply)
{
  return endpoint_invoke(caller, 0, 0, bytes, size, grants, grant_count,
      deadline_ns, reply);
}

enum call_status endpoint_notify(handle_t caller, uint64_t protocol,
    uint64_t operation, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count)
{
  struct endpoint_message message;
  enum call_status status = make_message(&message, PROTOCOL_ENDPOINT, ENDPOINT_SEND,
      protocol, operation, bytes, size, grants, grant_count);
  if (status != CALL_OK) {
    return status;
  }
  struct syscall_result result = syscall_call(caller, &message, sizeof(message), NULL, 0);
  status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}

enum call_status endpoint_send(handle_t caller, const void *bytes, size_t size,
    const struct endpoint_grant *grants, size_t grant_count)
{
  return endpoint_notify(caller, 0, 0, bytes, size, grants, grant_count);
}

enum call_status endpoint_receive(handle_t receiver, struct endpoint_packet *request)
{
  if (!request) {
    return CALL_BAD_REQUEST;
  }
  struct message_header message = {PROTOCOL_ENDPOINT_RECEIVER, ENDPOINT_RECEIVE};
  struct endpoint_packet packet = {0};
  struct syscall_result result = syscall_call(receiver, &message, sizeof(message),
      &packet, sizeof(packet));
  enum call_status status = checked_status(result);
  if (status != CALL_OK) {
    *request = (struct endpoint_packet){0};
    return result.reply_size == 0 ? status : CALL_BAD_REQUEST;
  }
  if (!packet_valid(&packet, result.reply_size, true, 0, 0, 0)) {
    *request = (struct endpoint_packet){0};
    return CALL_OUTCOME_UNKNOWN;
  }
  *request = packet;
  return CALL_OK;
}

enum call_status endpoint_reply(handle_t receipt, uint64_t application_result,
    const void *bytes, size_t size, const struct endpoint_grant *grants,
    size_t grant_count)
{
  struct endpoint_message message;
  enum call_status status = make_message(&message, PROTOCOL_ENDPOINT_RECEIPT,
      ENDPOINT_REPLY, 0, 0, bytes, size, grants, grant_count);
  if (status != CALL_OK) {
    return status;
  }
  message.result = application_result;
  struct syscall_result result = syscall_call(receipt, &message, sizeof(message), NULL, 0);
  status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}

enum call_status endpoint_finish(handle_t receipt)
{
  struct syscall_result result = syscall_close(receipt);
  enum call_status status = checked_status(result);
  return result.reply_size == 0 ? status : CALL_OUTCOME_UNKNOWN;
}
