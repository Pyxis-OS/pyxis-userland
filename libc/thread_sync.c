#include <clock.h>
#include <errno.h>
#include <stdlib.h>
#include <thread.h>
#include "errors.h"
#include "thread_platform.h"
#include "thread_sync.h"

#define SYNC_INVALID UINT32_MAX
#define RWLOCK_WRITER (UINT32_C(1) << 31)
#define RWLOCK_READERS (RWLOCK_WRITER - 1)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

static void notify(uint32_t *word, uint64_t maximum)
{
  /* Notification follows a committed userspace transition. Refusal cannot
   * strand other threads behind state which the caller believes published. */
  if (libc_wake(word, maximum)) {
    _Exit(EXIT_FAILURE);
  }
}

int libc_park(uint32_t *word, uint32_t expected, uint64_t deadline)
{
  handle_t service = libc_thread_service();
  if (service == HANDLE_INVALID) {
    return EACCES;
  }
  enum call_status status = thread_address_wait(service, (uintptr_t)word,
      expected, THREAD_ADDRESS_WORD32_BYTES, deadline);
  return status == CALL_WOULD_BLOCK ? 0 : libc_call_errno(status);
}

int libc_wake(uint32_t *word, uint64_t maximum)
{
  handle_t service = libc_thread_service();
  if (service == HANDLE_INVALID) {
    return EACCES;
  }
  uint64_t woken;
  return libc_call_errno(thread_address_wake(service, (uintptr_t)word,
      THREAD_ADDRESS_WORD32_BYTES, maximum, &woken));
}

void libc_lock_acquire(struct libc_lock *lock)
{
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&lock->word, &expected, 1, false,
      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return;
  }
  for (;;) {
    if (!__atomic_exchange_n(&lock->word, 2, __ATOMIC_ACQUIRE)) {
      return;
    }
    if (libc_park(&lock->word, 2, THREAD_ADDRESS_NO_DEADLINE)) {
      _Exit(EXIT_FAILURE);
    }
  }
}

void libc_lock_release(struct libc_lock *lock)
{
  if (__atomic_exchange_n(&lock->word, 0, __ATOMIC_RELEASE) == 2) {
    notify(&lock->word, 1);
  }
}

int libc_sync_validate_time(const struct timespec *time, clockid_t clock)
{
  if (!time || time->tv_nsec < 0 || time->tv_nsec >= (long)NANOSECONDS_PER_SECOND ||
      (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
    return EINVAL;
  }
  return 0;
}

static int native_deadline(const struct timespec *time, clockid_t clock,
    uint64_t *deadline)
{
  __int128 target = (__int128)time->tv_sec * NANOSECONDS_PER_SECOND + time->tv_nsec;
  __int128 remaining = target;
  handle_t service = libc_thread_clock();
  if (service == HANDLE_INVALID) {
    return EACCES;
  }
  if (clock == CLOCK_REALTIME) {
    struct clock_wall_reading wall;
    enum call_status status = clock_wall_now(service, &wall);
    if (status != CALL_OK) {
      return libc_call_errno(status);
    }
    remaining -= (__int128)wall.seconds * NANOSECONDS_PER_SECOND + wall.nanoseconds;
    if (remaining <= 0) {
      return ETIMEDOUT;
    }
  }
  uint64_t now;
  enum call_status status = clock_now(service, &now);
  if (status != CALL_OK) {
    return libc_call_errno(status);
  }
  if (clock == CLOCK_MONOTONIC) {
    if (target <= now) {
      return ETIMEDOUT;
    }
    if (target >= THREAD_ADDRESS_NO_DEADLINE) {
      return EOVERFLOW;
    }
    *deadline = target;
  } else {
    /* Wall is read first. Read-to-read delay can postpone wakeup, but cannot
     * make this translation report an early wall-clock timeout. */
    if (remaining >= (__int128)THREAD_ADDRESS_NO_DEADLINE - now) {
      return EOVERFLOW;
    }
    *deadline = now + (uint64_t)remaining;
  }
  return 0;
}

int libc_sync_wait(uint32_t *word, uint32_t expected,
    const struct timespec *time, clockid_t clock)
{
  if (!time) {
    return libc_park(word, expected, THREAD_ADDRESS_NO_DEADLINE);
  }
  int error = libc_sync_validate_time(time, clock);
  if (error) {
    return error;
  }
  for (;;) {
    uint64_t deadline;
    error = native_deadline(time, clock, &deadline);
    if (error) {
      return error;
    }
    error = libc_park(word, expected, deadline);
    if (error != ETIMEDOUT || clock == CLOCK_MONOTONIC) {
      return error;
    }
    /* Recheck UTC after native expiry instead of treating uptime as UTC. */
  }
}

int libc_mutex_init(struct libc_mutex *mutex, int type)
{
  if (!mutex || type < 0 || (type & ~(LIBC_MUTEX_RECURSIVE | LIBC_MUTEX_TIMED))) {
    return EINVAL;
  }
  *mutex = (struct libc_mutex){.type = type};
  return 0;
}

int libc_mutex_destroy(struct libc_mutex *mutex)
{
  if (!mutex) {
    return EINVAL;
  }
  libc_lock_acquire(&mutex->guard);
  int error = mutex->type == SYNC_INVALID ? EINVAL :
      mutex->depth || mutex->waiters ? EBUSY : 0;
  if (!error) {
    mutex->type = SYNC_INVALID;
  }
  libc_lock_release(&mutex->guard);
  return error;
}

static int lock_mutex(struct libc_mutex *mutex, bool try_only,
    const struct timespec *time, clockid_t clock)
{
  if (!mutex) {
    return EINVAL;
  }
  if (time) {
    int error = libc_sync_validate_time(time, clock);
    if (error) {
      return error;
    }
  }
  uint64_t self = libc_thread_self();
  for (;;) {
    libc_lock_acquire(&mutex->guard);
    int error = 0;
    if (mutex->type == SYNC_INVALID) {
      error = EINVAL;
    } else if (!mutex->depth) {
      mutex->owner = self;
      mutex->depth = 1;
      __atomic_store_n(&mutex->held, 1, __ATOMIC_RELAXED);
    } else if ((mutex->type & LIBC_MUTEX_RECURSIVE) && mutex->owner == self) {
      if (mutex->depth == UINT32_MAX) {
        error = EAGAIN;
      } else {
        mutex->depth++;
      }
    } else if (try_only) {
      error = EBUSY;
    } else if (mutex->waiters == UINT32_MAX) {
      error = EAGAIN;
    } else {
      mutex->waiters++;
      libc_lock_release(&mutex->guard);
      error = libc_sync_wait(&mutex->held, 1, time, clock);
      libc_lock_acquire(&mutex->guard);
      mutex->waiters--;
      libc_lock_release(&mutex->guard);
      if (error) {
        return error;
      }
      continue;
    }
    libc_lock_release(&mutex->guard);
    return error;
  }
}

int libc_mutex_lock(struct libc_mutex *mutex)
{
  return lock_mutex(mutex, false, NULL, CLOCK_MONOTONIC);
}

int libc_mutex_trylock(struct libc_mutex *mutex)
{
  return lock_mutex(mutex, true, NULL, CLOCK_MONOTONIC);
}

int libc_mutex_timedlock(struct libc_mutex *mutex, const struct timespec *time,
    clockid_t clock)
{
  if (!time) {
    return EINVAL;
  }
  return lock_mutex(mutex, false, time, clock);
}

int libc_mutex_unlock(struct libc_mutex *mutex)
{
  if (!mutex) {
    return EINVAL;
  }
  libc_lock_acquire(&mutex->guard);
  int error = 0;
  bool wake = false;
  if (mutex->type == SYNC_INVALID) {
    error = EINVAL;
  } else if (!mutex->depth || mutex->owner != libc_thread_self()) {
    error = EPERM;
  } else if (!--mutex->depth) {
    mutex->owner = 0;
    __atomic_store_n(&mutex->held, 0, __ATOMIC_RELEASE);
    wake = mutex->waiters != 0;
  }
  libc_lock_release(&mutex->guard);
  if (wake) {
    notify(&mutex->held, 1);
  }
  return error;
}

struct condition_waiter {
  struct condition_waiter *next;
  uint32_t notified;
};

int libc_condition_init(struct libc_condition *condition, clockid_t clock)
{
  if (!condition || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
    return EINVAL;
  }
  *condition = (struct libc_condition){.clock = clock};
  return 0;
}

int libc_condition_destroy(struct libc_condition *condition)
{
  if (!condition) {
    return EINVAL;
  }
  libc_lock_acquire(&condition->guard);
  int error = condition->clock < 0 ? EINVAL : condition->waiters ? EBUSY : 0;
  if (!error) {
    condition->clock = -1;
  }
  libc_lock_release(&condition->guard);
  return error;
}

static void remove_condition_waiter(struct libc_condition *condition,
    struct condition_waiter *waiter)
{
  struct condition_waiter *previous = NULL;
  struct condition_waiter *current = (void *)condition->first;
  while (current && current != waiter) {
    previous = current;
    current = current->next;
  }
  if (current) {
    if (previous) {
      previous->next = current->next;
    } else {
      condition->first = (uintptr_t)current->next;
    }
    if (condition->last == (uintptr_t)current) {
      condition->last = (uintptr_t)previous;
    }
  }
}

int libc_condition_wait(struct libc_condition *condition, struct libc_mutex *mutex,
    const struct timespec *time)
{
  if (!condition || !mutex) {
    return EINVAL;
  }
  struct condition_waiter waiter = {0};
  libc_lock_acquire(&condition->guard);
  int error = condition->clock < 0 ? EINVAL : 0;
  if (!error && time) {
    error = libc_sync_validate_time(time, condition->clock);
  }
  if (!error && condition->waiters && condition->mutex != (uintptr_t)mutex) {
    error = EINVAL;
  }
  if (!error && condition->waiters == UINT32_MAX) {
    error = EAGAIN;
  }
  libc_lock_acquire(&mutex->guard);
  if (!error && (mutex->type == SYNC_INVALID || mutex->depth != 1 ||
      mutex->owner != libc_thread_self())) {
    error = EINVAL;
  }
  libc_lock_release(&mutex->guard);
  if (error) {
    libc_lock_release(&condition->guard);
    return error;
  }
  struct condition_waiter *last = (void *)condition->last;
  if (last) {
    last->next = &waiter;
  } else {
    condition->first = (uintptr_t)&waiter;
  }
  condition->last = (uintptr_t)&waiter;
  condition->mutex = (uintptr_t)mutex;
  condition->waiters++;
  clockid_t clock = condition->clock;
  /* Enrol before unlocking. A notification publishes the private word before
   * wake, so notification between unlock and native WAIT cannot be lost. */
  error = libc_mutex_unlock(mutex);
  libc_lock_release(&condition->guard);
  if (error) {
    _Exit(EXIT_FAILURE);
  }
  while (!__atomic_load_n(&waiter.notified, __ATOMIC_ACQUIRE)) {
    error = libc_sync_wait(&waiter.notified, 0, time, clock);
    if (error) {
      break;
    }
  }
  if (libc_mutex_lock(mutex)) {
    _Exit(EXIT_FAILURE);
  }
  libc_lock_acquire(&condition->guard);
  if (__atomic_load_n(&waiter.notified, __ATOMIC_ACQUIRE)) {
    error = 0;
  } else {
    remove_condition_waiter(condition, &waiter);
  }
  if (!--condition->waiters) {
    condition->mutex = 0;
  }
  libc_lock_release(&condition->guard);
  return error;
}

int libc_condition_signal(struct libc_condition *condition, bool all)
{
  if (!condition) {
    return EINVAL;
  }
  libc_lock_acquire(&condition->guard);
  if (condition->clock < 0) {
    libc_lock_release(&condition->guard);
    return EINVAL;
  }
  do {
    struct condition_waiter *waiter = (void *)condition->first;
    if (!waiter) {
      break;
    }
    condition->first = (uintptr_t)waiter->next;
    if (!condition->first) {
      condition->last = 0;
    }
    __atomic_store_n(&waiter->notified, 1, __ATOMIC_RELEASE);
    /* The waiter cannot leave its stack until it reacquires this guard. */
    notify(&waiter->notified, 1);
  } while (all);
  libc_lock_release(&condition->guard);
  return 0;
}

int libc_once_begin(struct libc_once *once, struct libc_once_frame *frame, int *run)
{
  if (!once || !frame || !run) {
    return EINVAL;
  }
  *run = 0;
  for (;;) {
    libc_lock_acquire(&once->guard);
    uint32_t state = __atomic_load_n(&once->state, __ATOMIC_RELAXED);
    if (state == 2) {
      libc_lock_release(&once->guard);
      return 0;
    }
    if (state > 2) {
      libc_lock_release(&once->guard);
      return EINVAL;
    }
    if (!state) {
      __atomic_store_n(&once->state, 1, __ATOMIC_RELAXED);
      libc_lock_release(&once->guard);
      *frame = (struct libc_once_frame){.object = once};
      libc_thread_once_push(frame);
      *run = 1;
      return 0;
    }
    if (once->waiters == UINT32_MAX) {
      libc_lock_release(&once->guard);
      return EAGAIN;
    }
    once->waiters++;
    libc_lock_release(&once->guard);
    int error = libc_park(&once->state, 1, THREAD_ADDRESS_NO_DEADLINE);
    libc_lock_acquire(&once->guard);
    once->waiters--;
    libc_lock_release(&once->guard);
    if (error) {
      return error;
    }
  }
}

static void finish_once(struct libc_once_frame *frame, uint32_t state)
{
  struct libc_once *once = frame->object;
  libc_thread_once_pop(frame);
  libc_lock_acquire(&once->guard);
  __atomic_store_n(&once->state, state, __ATOMIC_RELEASE);
  bool wake = once->waiters != 0;
  libc_lock_release(&once->guard);
  if (wake) {
    notify(&once->state, UINT64_MAX);
  }
}

void libc_once_complete(struct libc_once_frame *frame)
{
  finish_once(frame, 2);
}

void libc_once_abort(struct libc_once_frame *frame)
{
  finish_once(frame, 0);
}

int libc_once_run(struct libc_once *once, void (*initialize)(void))
{
  if (!initialize) {
    return EINVAL;
  }
  struct libc_once_frame frame;
  int run;
  int error = libc_once_begin(once, &frame, &run);
  if (error || !run) {
    return error;
  }
  initialize();
  libc_once_complete(&frame);
  return 0;
}

void libc_once_thread_cleanup(void)
{
  struct libc_once_frame *frame;
  while ((frame = libc_thread_once_head())) {
    libc_once_abort(frame);
  }
}

int libc_rwlock_init(struct libc_rwlock *lock)
{
  if (!lock) {
    return EINVAL;
  }
  *lock = (struct libc_rwlock)LIBC_RWLOCK_INITIALIZER;
  return 0;
}

int libc_rwlock_destroy(struct libc_rwlock *lock)
{
  if (!lock) {
    return EINVAL;
  }
  libc_lock_acquire(&lock->guard);
  int error = !lock->valid ? EINVAL : lock->state || lock->waiters ? EBUSY : 0;
  if (!error) {
    lock->valid = 0;
  }
  libc_lock_release(&lock->guard);
  return error;
}

int libc_rwlock_lock(struct libc_rwlock *lock, bool write, bool try_only)
{
  if (!lock) {
    return EINVAL;
  }
  for (;;) {
    libc_lock_acquire(&lock->guard);
    uint32_t state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
    int error = 0;
    if (!lock->valid) {
      error = EINVAL;
    } else if ((state & RWLOCK_WRITER) && lock->owner == libc_thread_self()) {
      error = try_only ? EBUSY : EDEADLK;
    } else if (write ? !state : !(state & RWLOCK_WRITER)) {
      if (!write && state == RWLOCK_READERS) {
        error = EAGAIN;
      } else {
        __atomic_store_n(&lock->state, write ? RWLOCK_WRITER : state + 1,
            __ATOMIC_RELAXED);
        if (write) {
          lock->owner = libc_thread_self();
        }
      }
    } else if (try_only) {
      error = EBUSY;
    } else if (lock->waiters == UINT32_MAX) {
      error = EAGAIN;
    } else {
      lock->waiters++;
      libc_lock_release(&lock->guard);
      error = libc_park(&lock->state, state, THREAD_ADDRESS_NO_DEADLINE);
      libc_lock_acquire(&lock->guard);
      lock->waiters--;
      libc_lock_release(&lock->guard);
      if (error) {
        return error;
      }
      continue;
    }
    libc_lock_release(&lock->guard);
    return error;
  }
}

int libc_rwlock_unlock(struct libc_rwlock *lock)
{
  if (!lock) {
    return EINVAL;
  }
  libc_lock_acquire(&lock->guard);
  uint32_t state = __atomic_load_n(&lock->state, __ATOMIC_RELAXED);
  int error = 0;
  bool wake = false;
  if (!lock->valid) {
    error = EINVAL;
  } else if (!state || ((state & RWLOCK_WRITER) && lock->owner != libc_thread_self())) {
    error = EPERM;
  } else {
    __atomic_store_n(&lock->state, state & RWLOCK_WRITER ? 0 : state - 1,
        __ATOMIC_RELEASE);
    lock->owner = 0;
    wake = lock->waiters != 0;
  }
  libc_lock_release(&lock->guard);
  if (wake) {
    notify(&lock->state, UINT64_MAX);
  }
  return error;
}
