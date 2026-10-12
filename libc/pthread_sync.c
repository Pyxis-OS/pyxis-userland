#include <errno.h>
#include <pthread.h>
#include "thread_sync.h"

int pthread_mutexattr_init(pthread_mutexattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  *attributes = (pthread_mutexattr_t){PTHREAD_MUTEX_NORMAL, PTHREAD_PROCESS_PRIVATE};
  return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  attributes->type = -1;
  return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type)
{
  if (!attributes || attributes->type < 0) {
    return EINVAL;
  }
  if (type == PTHREAD_MUTEX_ERRORCHECK) {
    return ENOTSUP;
  }
  if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE) {
    return EINVAL;
  }
  attributes->type = type;
  return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type)
{
  if (!attributes || !type || attributes->type < 0) {
    return EINVAL;
  }
  *type = attributes->type;
  return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attributes, int shared)
{
  if (!attributes || attributes->type < 0) {
    return EINVAL;
  }
  if (shared == PTHREAD_PROCESS_SHARED) {
    return ENOTSUP;
  }
  if (shared != PTHREAD_PROCESS_PRIVATE) {
    return EINVAL;
  }
  attributes->process_shared = shared;
  return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attributes, int *shared)
{
  if (!attributes || !shared || attributes->type < 0) {
    return EINVAL;
  }
  *shared = attributes->process_shared;
  return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes)
{
  if (attributes && attributes->process_shared != PTHREAD_PROCESS_PRIVATE) {
    return ENOTSUP;
  }
  int type = attributes ? attributes->type : PTHREAD_MUTEX_NORMAL;
  if (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE) {
    return EINVAL;
  }
  return libc_mutex_init(mutex, type);
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
  return libc_mutex_destroy(mutex);
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
  return libc_mutex_lock(mutex);
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
  return libc_mutex_trylock(mutex);
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *time)
{
  return libc_mutex_timedlock(mutex, time, CLOCK_REALTIME);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
  return libc_mutex_unlock(mutex);
}

int pthread_condattr_init(pthread_condattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  *attributes = (pthread_condattr_t){CLOCK_REALTIME, PTHREAD_PROCESS_PRIVATE};
  return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  attributes->clock = -1;
  return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *attributes, clockid_t clock)
{
  if (!attributes || attributes->clock < 0 ||
      (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
    return EINVAL;
  }
  attributes->clock = clock;
  return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *attributes, clockid_t *clock)
{
  if (!attributes || !clock || attributes->clock < 0) {
    return EINVAL;
  }
  *clock = attributes->clock;
  return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *attributes, int shared)
{
  if (!attributes || attributes->clock < 0) {
    return EINVAL;
  }
  if (shared == PTHREAD_PROCESS_SHARED) {
    return ENOTSUP;
  }
  if (shared != PTHREAD_PROCESS_PRIVATE) {
    return EINVAL;
  }
  attributes->process_shared = shared;
  return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *attributes, int *shared)
{
  if (!attributes || !shared || attributes->clock < 0) {
    return EINVAL;
  }
  *shared = attributes->process_shared;
  return 0;
}

int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes)
{
  if (attributes && attributes->process_shared != PTHREAD_PROCESS_PRIVATE) {
    return ENOTSUP;
  }
  return libc_condition_init(condition, attributes ? attributes->clock : CLOCK_REALTIME);
}

int pthread_cond_destroy(pthread_cond_t *condition)
{
  return libc_condition_destroy(condition);
}

int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
  return libc_condition_wait(condition, mutex, NULL);
}

int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
    const struct timespec *time)
{
  if (!time) {
    return EINVAL;
  }
  return libc_condition_wait(condition, mutex, time);
}

int pthread_cond_signal(pthread_cond_t *condition)
{
  return libc_condition_signal(condition, false);
}

int pthread_cond_broadcast(pthread_cond_t *condition)
{
  return libc_condition_signal(condition, true);
}

int pthread_once(pthread_once_t *once, void (*initialize)(void))
{
  return libc_once_run(once, initialize);
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  *attributes = (pthread_rwlockattr_t){PTHREAD_PROCESS_PRIVATE};
  return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  attributes->process_shared = -1;
  return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attributes, int shared)
{
  if (!attributes || attributes->process_shared < 0) {
    return EINVAL;
  }
  if (shared == PTHREAD_PROCESS_SHARED) {
    return ENOTSUP;
  }
  if (shared != PTHREAD_PROCESS_PRIVATE) {
    return EINVAL;
  }
  attributes->process_shared = shared;
  return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attributes, int *shared)
{
  if (!attributes || !shared || attributes->process_shared < 0) {
    return EINVAL;
  }
  *shared = attributes->process_shared;
  return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attributes)
{
  if (attributes && attributes->process_shared != PTHREAD_PROCESS_PRIVATE) {
    return attributes->process_shared == PTHREAD_PROCESS_SHARED ? ENOTSUP : EINVAL;
  }
  return libc_rwlock_init(lock);
}

int pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
  return libc_rwlock_destroy(lock);
}

int pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
  return libc_rwlock_lock(lock, false, false);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *lock)
{
  return libc_rwlock_lock(lock, true, false);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock)
{
  return libc_rwlock_lock(lock, false, true);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *lock)
{
  return libc_rwlock_lock(lock, true, true);
}

int pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
  return libc_rwlock_unlock(lock);
}
