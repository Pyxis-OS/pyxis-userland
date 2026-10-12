#include <errno.h>
#include <stdlib.h>
#include <threads.h>
#include "thread_sync.h"

static int thread_status(int error)
{
  switch (error) {
  case 0: return thrd_success;
  case ENOMEM: return thrd_nomem;
  case EBUSY: return thrd_busy;
  case ETIMEDOUT: return thrd_timedout;
  default: return thrd_error;
  }
}

int mtx_init(mtx_t *mutex, int type)
{
  if (type < 0 || (type & ~(mtx_recursive | mtx_timed))) {
    return thrd_error;
  }
  return thread_status(libc_mutex_init(mutex,
      (type & mtx_recursive ? LIBC_MUTEX_RECURSIVE : LIBC_MUTEX_NORMAL) |
      (type & mtx_timed ? LIBC_MUTEX_TIMED : 0)));
}

void mtx_destroy(mtx_t *mutex)
{
  if (libc_mutex_destroy(mutex)) {
    _Exit(EXIT_FAILURE);
  }
}

int mtx_lock(mtx_t *mutex)
{
  return thread_status(libc_mutex_lock(mutex));
}

int mtx_trylock(mtx_t *mutex)
{
  return thread_status(libc_mutex_trylock(mutex));
}

int mtx_timedlock(mtx_t *mutex, const struct timespec *time)
{
  if (!mutex) {
    return thrd_error;
  }
  libc_lock_acquire(&mutex->guard);
  bool timed = mutex->type != UINT32_MAX && (mutex->type & LIBC_MUTEX_TIMED);
  libc_lock_release(&mutex->guard);
  return timed ? thread_status(libc_mutex_timedlock(mutex, time, CLOCK_REALTIME)) : thrd_error;
}

int mtx_unlock(mtx_t *mutex)
{
  return thread_status(libc_mutex_unlock(mutex));
}

int cnd_init(cnd_t *condition)
{
  return thread_status(libc_condition_init(condition, CLOCK_REALTIME));
}

void cnd_destroy(cnd_t *condition)
{
  if (libc_condition_destroy(condition)) {
    _Exit(EXIT_FAILURE);
  }
}

int cnd_wait(cnd_t *condition, mtx_t *mutex)
{
  return thread_status(libc_condition_wait(condition, mutex, NULL));
}

int cnd_timedwait(cnd_t *condition, mtx_t *mutex, const struct timespec *time)
{
  return time ? thread_status(libc_condition_wait(condition, mutex, time)) : thrd_error;
}

int cnd_signal(cnd_t *condition)
{
  return thread_status(libc_condition_signal(condition, false));
}

int cnd_broadcast(cnd_t *condition)
{
  return thread_status(libc_condition_signal(condition, true));
}

void call_once(once_flag *once, void (*initialize)(void))
{
  if (libc_once_run(once, initialize)) {
    _Exit(EXIT_FAILURE);
  }
}
