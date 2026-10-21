#include <wifi_auth.h>
#include <handle.h>
#include <syscall.h>
#include <string.h>

static enum call_status invoke(handle_t auth, const void *request, size_t bytes,
    void *reply, size_t capacity)
{
  struct syscall_result result = syscall_call(auth, request, bytes, reply, capacity);
  if (result.status >= CALL_STATUS_COUNT) {
    return CALL_UNAVAILABLE;
  }
  return result.reply_size == (result.status == CALL_OK ? capacity : 0) ?
      result.status : CALL_BAD_REQUEST;
}

enum call_status wifi_auth_open(handle_t radio, struct wifi_auth_open_reply *pair)
{
  if (!pair) return CALL_BAD_REQUEST;
  *pair = (struct wifi_auth_open_reply){0};
  struct message_header request = {PROTOCOL_WIFI, WIFI_OPEN_AUTH};
  struct wifi_auth_open_reply reply = {0};
  enum call_status status = invoke(radio, &request, sizeof(request), &reply, sizeof(reply));
  if (status == CALL_OK) {
    if (!reply.auth || !reply.observation || reply.auth == reply.observation) {
      if (reply.auth) handle_close(reply.auth);
      if (reply.observation && reply.observation != reply.auth) handle_close(reply.observation);
      return CALL_BAD_REQUEST;
    }
    *pair = reply;
  }
  return status;
}

enum call_status wifi_auth_claim(handle_t auth)
{
  struct message_header request = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_CLAIM};
  return invoke(auth, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_auth_status(handle_t auth, struct wifi_auth_status_reply *status)
{
  if (!status) return CALL_BAD_REQUEST;
  *status = (struct wifi_auth_status_reply){0};
  struct message_header request = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_STATUS};
  struct wifi_auth_status_reply reply = {0};
  enum call_status result = invoke(auth, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    if (reply.reserved || reply.status >= CALL_STATUS_COUNT ||
        (reply.flags & ~(WIFI_AUTH_CLAIMED | WIFI_AUTH_CLOSED | WIFI_AUTH_ACTIVE | WIFI_AUTH_BUSY))) {
      return CALL_BAD_REQUEST;
    }
    *status = reply;
  }
  return result;
}

enum call_status wifi_auth_read(handle_t auth, struct wifi_auth_event *event)
{
  if (!event) return CALL_BAD_REQUEST;
  *event = (struct wifi_auth_event){0};
  struct message_header request = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_READ};
  struct wifi_auth_event reply = {0};
  enum call_status result = invoke(auth, &request, sizeof(request), &reply, sizeof(reply));
  if (result == CALL_OK) {
    bool valid = reply.generation && reply.status < CALL_STATUS_COUNT &&
        !(reply.flags & ~WIFI_AUTH_EVENT_ENCRYPTED);
    if (reply.kind == WIFI_AUTH_BEGIN) {
      valid = valid && reply.credential && !reply.flags &&
          reply.length == sizeof(struct wifi_auth_begin);
    } else if (reply.kind == WIFI_AUTH_EAPOL) {
      valid = valid && !reply.credential && reply.length && reply.length <= WIFI_AUTH_DATA_BYTES;
    } else if (reply.kind == WIFI_AUTH_DISASSOC) {
      valid = valid && !reply.credential && !reply.length && !reply.flags;
    } else {
      valid = false;
    }
    if (!valid) {
      if (reply.credential) handle_close(reply.credential);
      result = CALL_BAD_REQUEST;
    } else {
      *event = reply;
    }
  }
  memset_explicit(&reply, 0, sizeof(reply));
  return result;
}

enum call_status wifi_auth_send(handle_t auth, uint64_t generation,
    const void *data, size_t bytes, uint32_t flags)
{
  if (!data || !bytes || bytes > WIFI_AUTH_DATA_BYTES) return CALL_BAD_REQUEST;
  struct wifi_auth_send_request request = {
    .header = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_SEND},
    .generation = generation, .length = bytes, .flags = flags,
  };
  memcpy(request.data, data, bytes);
  enum call_status result = invoke(auth, &request, sizeof(request), NULL, 0);
  memset_explicit(&request, 0, sizeof(request));
  return result;
}

enum call_status wifi_auth_key(handle_t auth, uint64_t generation, bool group,
    uint32_t key_id, uint32_t action, uint64_t receive_pn, const uint8_t key[16])
{
  if (action == WIFI_AUTH_KEY_INSTALL && !key) return CALL_BAD_REQUEST;
  struct wifi_auth_key_request request = {
    .header = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_KEY}, .generation = generation,
    .receive_pn = receive_pn, .action = action, .key_id = key_id, .group = group,
  };
  if (key) memcpy(request.key, key, sizeof(request.key));
  enum call_status result = invoke(auth, &request, sizeof(request), NULL, 0);
  memset_explicit(&request, 0, sizeof(request));
  return result;
}

enum call_status wifi_auth_authorize(handle_t auth, uint64_t generation, bool enabled)
{
  struct wifi_auth_authorize_request request = {
    .header = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_AUTHORIZE},
    .generation = generation, .enabled = enabled,
  };
  return invoke(auth, &request, sizeof(request), NULL, 0);
}

enum call_status wifi_auth_fail(handle_t auth, uint64_t generation, enum call_status status)
{
  struct wifi_auth_fail_request request = {
    .header = {PROTOCOL_WIFI_AUTH, WIFI_AUTH_FAIL},
    .generation = generation, .status = status,
  };
  return invoke(auth, &request, sizeof(request), NULL, 0);
}
