#ifndef USERSPACE_PROVIDER_SETUP_H
#define USERSPACE_PROVIDER_SETUP_H

#include <clock.h>
#include <endpoint.h>
#include <handle.h>
#include <stdbool.h>

#define PROVIDER_SETUP_TIMEOUT_NS UINT64_C(10000000000)

/* Exactly one provider grant accompanies success; failure carries none.
 * The launcher acknowledges a reported failure without publishing anything.
 * Cleanup failures remain separate and cannot be ignored by optional startup.
 * Ready reporting requires a bounded clock. attempted changes only on actual
 * submission, so pre-submission failure can be cleaned up and then reported.
 * Failed setup uses an unlimited report if the clock itself is unavailable;
 * the launcher is already waiting for this one-use handshake. */
struct provider_setup {
  uint64_t status;
  uint64_t cleanup_status;
};

static inline enum call_status provider_setup_report(handle_t publication,
    handle_t clock, enum call_status status, enum call_status cleanup_status,
    const struct endpoint_grant *grant, bool *attempted)
{
  uint64_t now;
  uint64_t deadline = 0;
  enum call_status clock_status = clock_now(clock, &now);
  if (clock_status == CALL_OK && now > UINT64_MAX - PROVIDER_SETUP_TIMEOUT_NS) {
    clock_status = CALL_LIMIT;
  }
  if (clock_status == CALL_OK) {
    deadline = now + PROVIDER_SETUP_TIMEOUT_NS;
  } else if (grant) {
    /* The caller must release failed setup before reporting it. */
    return clock_status;
  }
  struct provider_setup setup = {status, cleanup_status};
  struct endpoint_packet response = {0};
  if (attempted) {
    *attempted = true;
  }
  enum call_status result = endpoint_request(publication, &setup, sizeof(setup),
      grant, grant ? 1 : 0, deadline, &response);
  for (size_t i = 0; i < response.grant_count; ++i) {
    if (handle_close(response.grants[i].handle) != 0 && result == CALL_OK) {
      result = CALL_BAD_HANDLE;
    }
  }
  if (result == CALL_OK && (response.result != CALL_OK || response.size != 0 ||
      response.grant_count != 0)) {
    result = response.result != CALL_OK && response.result < CALL_STATUS_COUNT ?
        response.result : CALL_BAD_REQUEST;
  }
  return result;
}

#endif
