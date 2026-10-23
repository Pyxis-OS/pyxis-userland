#include "provider_setup.h"
#include <abi/wait.h>
#include <stdint.h>
#include <wait.h>

enum call_status provider_setup_receive(handle_t clock, handle_t process, handle_t receiver,
    handle_t armed, struct endpoint_packet *packet)
{
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK || now > UINT64_MAX - PROVIDER_SETUP_TIMEOUT_NS) {
    return status == CALL_OK ? CALL_LIMIT : status;
  }
  uint64_t deadline = now + PROVIDER_SETUP_TIMEOUT_NS;
  for (;;) {
    struct wait_interest interests[] = {
      {.handle = receiver, .events = WAIT_READABLE},
      {.handle = process, .events = WAIT_COMPLETE},
      {.handle = armed, .events = WAIT_INTERRUPT},
    };
    uint64_t events[3] = {0};
    status = wait_many(interests, armed != HANDLE_INVALID ? 3 : 2, deadline, events);
    if (status != CALL_OK) {
      return status;
    }
    if (events[0]) {
      status = endpoint_try_receive(receiver, packet);
      if (status != CALL_WOULD_BLOCK) {
        return status;
      }
    }
    if (events[1]) {
      return CALL_IO;
    }
    if (armed != HANDLE_INVALID && (events[2] & (WAIT_INTERRUPT | WAIT_ERROR))) {
      return CALL_ABANDONED;
    }
  }
}

enum provider_setup_outcome provider_setup_read(const struct endpoint_packet *packet,
    struct provider_setup *setup)
{
  *setup = (struct provider_setup){0};
  if (packet->kind != ENDPOINT_MESSAGE_CALL || packet->protocol != 0 ||
      packet->operation != 0 || packet->object_id != 0 || packet->reason != 0 ||
      packet->size != sizeof(*setup)) {
    return PROVIDER_SETUP_INVALID;
  }
  memcpy(setup, packet->data, sizeof(*setup));
  if (setup->status >= CALL_STATUS_COUNT || setup->cleanup_status >= CALL_STATUS_COUNT) {
    return PROVIDER_SETUP_INVALID;
  }
  if (setup->status == CALL_OK && setup->cleanup_status == CALL_OK &&
      setup->root_count && setup->root_count <= PROVIDER_SETUP_ROOT_MAX &&
      packet->grant_count == setup->root_count) {
    return PROVIDER_SETUP_READY;
  }
  if (setup->status != CALL_OK && !setup->root_count && packet->grant_count == 0) {
    return PROVIDER_SETUP_FAILED;
  }
  return PROVIDER_SETUP_INVALID;
}

enum call_status provider_setup_match(const struct provider_setup *setup,
    const struct endpoint_packet *packet, const char *const *roles, size_t role_count,
    size_t *selected)
{
  for (size_t i = 0; i < role_count; ++i) {
    selected[i] = SIZE_MAX;
  }
  for (size_t i = 0; i < setup->root_count; ++i) {
    const char *role = setup->roles[i];
    size_t length = 0;
    while (length <= PROVIDER_SETUP_ROLE_MAX && role[length]) {
      ++length;
    }
    if (!length || length > PROVIDER_SETUP_ROLE_MAX) {
      return CALL_BAD_REQUEST;
    }
    /* Fixed strings are zero padded; reject hidden suffix bytes. */
    for (size_t j = length + 1; j <= PROVIDER_SETUP_ROLE_MAX; ++j) {
      if (role[j]) {
        return CALL_BAD_REQUEST;
      }
    }
    size_t match = role_count;
    for (size_t j = 0; j < role_count; ++j) {
      if (!strcmp(role, roles[j])) {
        match = j;
      }
    }
    if (match == role_count || selected[match] != SIZE_MAX) {
      return CALL_BAD_REQUEST;
    }
    const struct endpoint_grant *grant = &packet->grants[i];
    struct handle_info info;
    enum call_status status = handle_query(grant->handle, &info);
    if (status != CALL_OK) {
      return status;
    }
    if (info.kind != HANDLE_KIND_EXPORTED || grant->transport != HANDLE_TRANSPORT_CALL ||
        info.rights != grant->rights || info.transport != grant->transport) {
      return CALL_BAD_REQUEST;
    }
    selected[match] = i;
  }
  return CALL_OK;
}
