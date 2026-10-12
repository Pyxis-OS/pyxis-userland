#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <thread.h>
#include "thread_sync.h"

static int result(int error)
{
  if (error) {
    errno = error;
    return -1;
  }
  return 0;
}

int sem_init(sem_t *semaphore, int shared, unsigned int value)
{
  if (!semaphore || value > SEM_VALUE_MAX) {
    return result(EINVAL);
  }
  if (shared) {
    return result(ENOTSUP);
  }
  *semaphore = (sem_t){.count = value, .valid = 1};
  return 0;
}

int sem_destroy(sem_t *semaphore)
{
  if (!semaphore) {
    return result(EINVAL);
  }
  libc_lock_acquire(&semaphore->guard);
  int error = !semaphore->valid ? EINVAL : semaphore->waiters ? EBUSY : 0;
  if (!error) {
    semaphore->valid = 0;
  }
  libc_lock_release(&semaphore->guard);
  return result(error);
}

static int wait_semaphore(sem_t *semaphore, const struct timespec *time, bool try_only)
{
  if (!semaphore) {
    return result(EINVAL);
  }
  if (time) {
    int error = libc_sync_validate_time(time, CLOCK_REALTIME);
    if (error) {
      return result(error);
    }
  }
  for (;;) {
    libc_lock_acquire(&semaphore->guard);
    int error = 0;
    if (!semaphore->valid) {
      error = EINVAL;
    } else if (semaphore->count) {
      __atomic_store_n(&semaphore->count, semaphore->count - 1, __ATOMIC_RELAXED);
    } else if (try_only) {
      error = EAGAIN;
    } else if (semaphore->waiters == UINT32_MAX) {
      error = EAGAIN;
    } else {
      semaphore->waiters++;
      libc_lock_release(&semaphore->guard);
      error = libc_sync_wait(&semaphore->count, 0, time, CLOCK_REALTIME);
      libc_lock_acquire(&semaphore->guard);
      semaphore->waiters--;
      libc_lock_release(&semaphore->guard);
      if (error) {
        return result(error);
      }
      continue;
    }
    libc_lock_release(&semaphore->guard);
    return result(error);
  }
}

int sem_wait(sem_t *semaphore)
{
  return wait_semaphore(semaphore, NULL, false);
}

int sem_trywait(sem_t *semaphore)
{
  return wait_semaphore(semaphore, NULL, true);
}

int sem_timedwait(sem_t *semaphore, const struct timespec *time)
{
  return time ? wait_semaphore(semaphore, time, false) : result(EINVAL);
}

int sem_post(sem_t *semaphore)
{
  if (!semaphore) {
    return result(EINVAL);
  }
  libc_lock_acquire(&semaphore->guard);
  int error = 0;
  bool wake = false;
  if (!semaphore->valid) {
    error = EINVAL;
  } else if (semaphore->count == SEM_VALUE_MAX) {
    error = EOVERFLOW;
  } else {
    __atomic_store_n(&semaphore->count, semaphore->count + 1, __ATOMIC_RELEASE);
    wake = semaphore->waiters != 0;
  }
  libc_lock_release(&semaphore->guard);
  if (wake && libc_wake(&semaphore->count, 1)) {
    _Exit(EXIT_FAILURE);
  }
  return result(error);
}

int sem_getvalue(sem_t *semaphore, int *value)
{
  if (!semaphore || !value) {
    return result(EINVAL);
  }
  libc_lock_acquire(&semaphore->guard);
  int error = semaphore->valid ? 0 : EINVAL;
  if (!error) {
    *value = semaphore->count;
  }
  libc_lock_release(&semaphore->guard);
  return result(error);
}
