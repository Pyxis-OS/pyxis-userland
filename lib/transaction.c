#include <transaction.h>
#include <abi/stream.h>
#include <endpoint.h>
#include <handle.h>
#include <string.h>

static void close_grants(struct endpoint_packet *packet)
{
  for (size_t i = 0; i < packet->grant_count; ++i) {
    if (packet->grants[i].handle != HANDLE_INVALID) {
      handle_close(packet->grants[i].handle);
    }
  }
}

static enum call_status check_control(handle_t control, uint64_t rights)
{
  struct handle_info info;
  enum call_status status = handle_query(control, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.protocol != PROTOCOL_TRANSACTION || info.kind != HANDLE_KIND_EXPORTED) {
    return CALL_WRONG_TYPE;
  }
  return (info.rights & rights) == rights &&
      (info.transport & HANDLE_TRANSPORT_CALL) ? CALL_OK : CALL_DENIED;
}

static bool grant_attested(const struct endpoint_grant *grant,
    const struct describe_reply *info)
{
  return grant->handle != HANDLE_INVALID && info->rights == grant->rights &&
      info->transport == grant->transport &&
      ((info->kind == HANDLE_KIND_NATIVE && !info->transport) ||
      (info->kind == HANDLE_KIND_EXPORTED &&
      (info->transport & HANDLE_TRANSPORT_CALL)));
}

enum call_status transaction_stage(handle_t control, uint64_t deadline_ns,
    handle_t *body)
{
  if (!body) {
    return CALL_BAD_REQUEST;
  }
  *body = HANDLE_INVALID;
  enum call_status status = check_control(control, TRANSACTION_RIGHT_STAGE);
  if (status != CALL_OK) {
    return status;
  }
  struct endpoint_packet packet = {0};
  status = endpoint_invoke(control, PROTOCOL_TRANSACTION, TRANSACTION_STAGE,
      NULL, 0, NULL, 0, deadline_ns, &packet);
  if (status == CALL_OK) {
    if (packet.result >= CALL_STATUS_COUNT || packet.size ||
        packet.grant_count != (packet.result == CALL_OK ? 1u : 0u)) {
      status = CALL_BAD_REQUEST;
    } else if (packet.result != CALL_OK) {
      status = packet.result;
    } else {
      struct resource_result captured = {0};
      status = resource_result_adopt(packet.grants[0].handle, &captured);
      if (status == CALL_OK) {
        if (!grant_attested(&packet.grants[0], &captured.info) ||
            captured.info.protocol != PROTOCOL_FILE ||
            !(captured.info.rights & FILE_RIGHT_WRITE)) {
          status = CALL_BAD_REQUEST;
        } else {
          *body = captured.resource;
          captured.resource = HANDLE_INVALID;
        }
        packet.grants[0].handle = HANDLE_INVALID;
        resource_result_close(&captured);
      }
    }
  }
  close_grants(&packet);
  return status;
}

enum call_status transaction_commit(handle_t control, uint64_t deadline_ns,
    struct resource_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct resource_result){0};
  enum call_status status = check_control(control, TRANSACTION_RIGHT_COMMIT);
  if (status != CALL_OK) {
    return status;
  }
  struct endpoint_packet packet = {0};
  status = endpoint_invoke(control, PROTOCOL_TRANSACTION, TRANSACTION_COMMIT,
      NULL, 0, NULL, 0, deadline_ns, &packet);
  result->delivery = packet.delivery;
  if (status != CALL_OK) {
    close_grants(&packet);
    return packet.delivery == ENDPOINT_DELIVERED ? CALL_OUTCOME_UNKNOWN : status;
  }
  struct provider_open_reply reply;
  if (packet.result >= CALL_STATUS_COUNT ||
      (packet.result != CALL_OK && (packet.size || packet.grant_count))) {
    status = CALL_OUTCOME_UNKNOWN;
  } else if (packet.result != CALL_OK) {
    status = packet.result;
  } else if (packet.size != sizeof(reply)) {
    status = CALL_OUTCOME_UNKNOWN;
  } else {
    memcpy(&reply, packet.data, sizeof(reply));
    if (!resource_description_valid(&reply.description) ||
        packet.grant_count != (reply.protocol ? 1u : 0u) ||
        (!reply.protocol && (reply.description.outcome == RESOURCE_OK ||
        (reply.description.flags & RESOURCE_ERROR_BODY)))) {
      status = CALL_OUTCOME_UNKNOWN;
    } else if (!reply.protocol) {
      result->info.description = reply.description;
    } else {
      struct resource_result captured = {0};
      status = resource_result_adopt(packet.grants[0].handle, &captured);
      if (status == CALL_OK) {
        if (!grant_attested(&packet.grants[0], &captured.info) ||
            captured.info.protocol != reply.protocol ||
            (reply.protocol != PROTOCOL_FILE && reply.protocol != PROTOCOL_STREAM) ||
            !(captured.info.rights & (reply.protocol == PROTOCOL_FILE ?
            FILE_RIGHT_READ : STREAM_RIGHT_READ)) ||
            captured.info.description.outcome != reply.description.outcome ||
            ((reply.description.flags & RESOURCE_ERROR_BODY) &&
            captured.info.error_body == HANDLE_INVALID)) {
          status = CALL_OUTCOME_UNKNOWN;
          packet.grants[0].handle = HANDLE_INVALID;
          resource_result_close(&captured);
        } else {
          captured.info.description = reply.description;
          captured.delivery = packet.delivery;
          *result = captured;
          packet.grants[0].handle = HANDLE_INVALID;
        }
      }
    }
  }
  close_grants(&packet);
  return status;
}

enum call_status transaction_abort(handle_t control, uint64_t deadline_ns)
{
  enum call_status status = check_control(control, TRANSACTION_RIGHT_ABORT);
  if (status != CALL_OK) {
    return status;
  }
  struct endpoint_packet packet = {0};
  status = endpoint_invoke(control, PROTOCOL_TRANSACTION, TRANSACTION_ABORT,
      NULL, 0, NULL, 0, deadline_ns, &packet);
  close_grants(&packet);
  return status != CALL_OK ? status : packet.grant_count || packet.size ||
      packet.result >= CALL_STATUS_COUNT ? CALL_BAD_REQUEST : packet.result;
}
