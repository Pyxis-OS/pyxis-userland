#include "rsn.h"
#include "../common/bootstrap_environment.h"
#include <clock.h>
#include <handle.h>
#include <wait.h>
#include <wifi_auth.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1) {
    return EXIT_FAILURE;
  }
  struct rsn_context context = {
    .auth = bootstrap_take_capability("wifi_auth"),
    .clock = bootstrap_capability("clock"),
    .random = bootstrap_capability("random"),
  };
  enum call_status status = !context.auth || !context.clock || !context.random ?
      CALL_DENIED : wifi_auth_claim(context.auth);
  while (status == CALL_OK) {
    /* Bound one drain so kernel events cannot starve an expired RSN timer. */
    for (size_t count = 0; count < 8 && status == CALL_OK; ++count) {
      struct wifi_auth_event event = {0};
      status = wifi_auth_read(context.auth, &event);
      if (status == CALL_WOULD_BLOCK) {
        status = CALL_OK;
        break;
      }
      if (status == CALL_OK) {
        if (event.kind == WIFI_AUTH_BEGIN) {
          status = rsn_begin(&context, &event);
          event.credential = HANDLE_INVALID; /* Consumed on every result. */
        } else if (event.kind == WIFI_AUTH_EAPOL) {
          if (context.sm && event.generation == context.generation) {
            status = rsn_eapol(&context, &event);
          }
        } else if (event.kind == WIFI_AUTH_DISASSOC) {
          if (event.generation == context.generation) {
            rsn_clear(&context);
          }
        } else {
          status = CALL_BAD_REQUEST;
        }
      }
      if (event.credential) {
        handle_close(event.credential);
      }
      memset_explicit(&event, 0, sizeof(event));
    }
    if (status == CALL_OK) {
      status = rsn_dispatch_timers(&context);
    }
    /* A failed association is reported through FAIL and may be followed by a
     * fresh BEGIN. Only a failed auth transport ends this trusted worker. */
    if (status != CALL_OK && context.failure_sent) {
      rsn_clear(&context);
      status = CALL_OK;
    }
    if (status != CALL_OK) {
      break;
    }
    struct wait_interest interest = {context.auth, WAIT_READABLE | WAIT_CLOSED, 0};
    uint64_t events = 0;
    uint64_t now;
    status = clock_now(context.clock, &now);
    if (status != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
      break;
    }
    uint64_t deadline = rsn_deadline(&context);
    if (deadline > now + WAIT_MAX_WAIT_NS) {
      deadline = now + WAIT_MAX_WAIT_NS;
    }
    status = wait_many(&interest, 1, deadline, &events);
    if (status == CALL_TIMED_OUT) {
      status = CALL_OK;
    }
    if (status == CALL_OK && (events & WAIT_CLOSED)) {
      status = CALL_ENDPOINT_CLOSED;
    }
  }
  rsn_clear(&context);
  if (context.auth) {
    handle_close(context.auth);
  }
  memset_explicit(&context, 0, sizeof(context));
  return status == CALL_ENDPOINT_CLOSED ? EXIT_SUCCESS : EXIT_FAILURE;
}
