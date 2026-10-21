#include "rsn.h"

#include <clock.h>
#include <random.h>
#include <stdlib.h>
#include <string.h>

static struct rsn_context *active_context;

struct rsn_context *rsn_os_context(void)
{
  return active_context;
}

void rsn_os_enter(struct rsn_context *context)
{
  active_context = context;
}

void *os_zalloc(size_t size)
{
  return calloc(1, size);
}

void *os_memdup(const void *source, size_t size)
{
  if (!source && size) {
    return NULL;
  }
  void *copy = malloc(size);
  if (copy && size) {
    memcpy(copy, source, size);
  }
  return copy;
}

int os_memcmp_const(const void *left, const void *right, size_t size)
{
  const uint8_t *a = left, *b = right;
  volatile uint8_t different = 0;
  for (size_t i = 0; i < size; ++i) {
    different = different | (a[i] ^ b[i]);
  }
  return different;
}

int os_get_random(unsigned char *bytes, size_t size)
{
  struct rsn_context *context = active_context;
  if (!context || !context->generation || context->failure != CALL_OK ||
      (!bytes && size)) {
    return -1;
  }
  if (size > RANDOM_MAX_BYTES) {
    rsn_fail(context, CALL_LIMIT);
    return -1;
  }
  uint64_t now;
  enum call_status status = clock_now(context->clock, &now);
  if (status == CALL_OK) {
    uint64_t deadline = now > UINT64_MAX - RANDOM_MAX_WAIT_NS ?
        UINT64_MAX : now + RANDOM_MAX_WAIT_NS;
    if (context->phase_deadline < deadline) {
      deadline = context->phase_deadline;
    }
    status = deadline <= now ? CALL_TIMED_OUT :
        random_read(context->random, bytes, size, deadline);
  }
  if (status != CALL_OK) {
    if (bytes && size) {
      memset_explicit(bytes, 0, size);
    }
    rsn_fail(context, status);
    return -1;
  }
  return 0;
}

int eloop_register_timeout(unsigned int seconds, unsigned int microseconds,
    eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
  struct rsn_context *context = active_context;
  if (!context || !handler || context->failure != CALL_OK) {
    return -1;
  }
  uint64_t now;
  enum call_status status = clock_now(context->clock, &now);
  uint64_t duration = (uint64_t)seconds * UINT64_C(1000000000) +
      (uint64_t)microseconds * UINT64_C(1000);
  if (status != CALL_OK || duration > UINT64_MAX - now) {
    rsn_fail(context, status == CALL_OK ? CALL_LIMIT : status);
    return -1;
  }
  for (size_t i = 0; i < RSN_TIMER_LIMIT; ++i) {
    if (!context->timers[i].active) {
      context->timers[i] = (struct rsn_timer){
        .active = true, .deadline = now + duration, .handler = handler,
        .eloop_data = eloop_data, .user_data = user_data,
      };
      return 0;
    }
  }
  rsn_fail(context, CALL_LIMIT);
  return -1;
}

int eloop_cancel_timeout(eloop_timeout_handler handler, void *eloop_data, void *user_data)
{
  struct rsn_context *context = active_context;
  if (!context) {
    return 0;
  }
  int cancelled = 0;
  for (size_t i = 0; i < RSN_TIMER_LIMIT; ++i) {
    struct rsn_timer *timer = &context->timers[i];
    if (timer->active && timer->handler == handler &&
        (eloop_data == ELOOP_ALL_CTX || timer->eloop_data == eloop_data) &&
        (user_data == ELOOP_ALL_CTX || timer->user_data == user_data)) {
      memset(timer, 0, sizeof(*timer));
      ++cancelled;
    }
  }
  return cancelled;
}
