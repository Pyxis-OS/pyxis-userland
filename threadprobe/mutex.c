#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool finished;
static atomic_uint ready;
static unsigned increments;

static void sleep_ms(long milliseconds)
{
  struct timespec duration = {.tv_sec = milliseconds / 1000,
      .tv_nsec = milliseconds % 1000 * 1000000};
  if (thrd_sleep(&duration, NULL)) {
    _Exit(1);
  }
}

static struct timespec deadline_ms(long milliseconds)
{
  struct timespec time;
  if (clock_gettime(CLOCK_REALTIME, &time)) {
    _Exit(1);
  }
  time.tv_sec += milliseconds / 1000;
  time.tv_nsec += milliseconds % 1000 * 1000000;
  if (time.tv_nsec >= 1000000000) {
    time.tv_nsec -= 1000000000;
    ++time.tv_sec;
  }
  return time;
}

static int watchdog(void *unused)
{
  (void)unused;
  for (unsigned i = 0; i < 300 && !atomic_load(&finished); ++i) {
    sleep_ms(100);
  }
  if (!atomic_load(&finished)) {
    _Exit(124);
  }
  return 0;
}

static int foreign_owner(void *unused)
{
  (void)unused;
  return pthread_mutex_unlock(&mutex) == EPERM &&
      pthread_mutex_trylock(&mutex) == EBUSY ? 0 : 1;
}

static int contender(void *argument)
{
  bool timed = argument != NULL;
  struct timespec deadline = deadline_ms(2);
  atomic_fetch_add(&ready, 1);
  int status = timed ? pthread_mutex_timedlock(&mutex, &deadline) : pthread_mutex_lock(&mutex);
  if (status == ETIMEDOUT && timed) {
    return ETIMEDOUT;
  }
  if (status) {
    _Exit(1);
  }
  ++increments;
  if (pthread_mutex_unlock(&mutex)) {
    _Exit(1);
  }
  return 0;
}

static void wait_ready(unsigned count)
{
  while (atomic_load(&ready) != count) {
    sleep_ms(1);
  }
}

int threadprobe_runtime_mutex_checks(void)
{
  thrd_t guard;
  if (thrd_create(&guard, watchdog, NULL) != thrd_success) {
    return 1;
  }
  bool valid = pthread_mutex_unlock(&mutex) == EPERM;
  valid &= pthread_mutex_trylock(&mutex) == 0;
  valid &= pthread_mutex_trylock(&mutex) == EBUSY;
  valid &= pthread_mutex_destroy(&mutex) == EBUSY;
  thrd_t foreign;
  if (thrd_create(&foreign, foreign_owner, NULL) != thrd_success) {
    _Exit(1);
  }
  int result;
  valid &= thrd_join(foreign, &result) == thrd_success && result == 0;
  valid &= pthread_mutex_unlock(&mutex) == 0;
  valid &= pthread_mutex_destroy(&mutex) == 0;
  valid &= pthread_mutex_lock(&mutex) == EINVAL;
  valid &= pthread_mutex_trylock(&mutex) == EINVAL;
  valid &= pthread_mutex_unlock(&mutex) == EINVAL;
  valid &= pthread_mutex_destroy(&mutex) == EINVAL;
  valid &= pthread_mutex_init(&mutex, NULL) == 0;

  pthread_mutex_t recursive = LIBC_RECURSIVE_MUTEX_INITIALIZER;
  valid &= pthread_mutex_trylock(&recursive) == 0;
  valid &= pthread_mutex_trylock(&recursive) == 0;
  valid &= pthread_mutex_destroy(&recursive) == EBUSY;
  valid &= pthread_mutex_unlock(&recursive) == 0;
  valid &= pthread_mutex_unlock(&recursive) == 0;
  valid &= pthread_mutex_unlock(&recursive) == EPERM;
  valid &= pthread_mutex_destroy(&recursive) == 0;

  mtx_t plain, timed;
  valid &= mtx_init(&plain, mtx_plain) == thrd_success;
  struct timespec deadline = deadline_ms(1000);
  valid &= mtx_timedlock(&plain, &deadline) == thrd_error;
  valid &= mtx_trylock(&plain) == thrd_success;
  valid &= mtx_trylock(&plain) == thrd_busy;
  valid &= mtx_unlock(&plain) == thrd_success;
  mtx_destroy(&plain);
  valid &= mtx_init(&timed, mtx_timed) == thrd_success;
  struct timespec invalid = {.tv_nsec = 1000000000};
  valid &= mtx_timedlock(&timed, &invalid) == thrd_error;
  valid &= mtx_timedlock(&timed, &deadline) == thrd_success;
  valid &= mtx_unlock(&timed) == thrd_success;
  mtx_destroy(&timed);
  valid &= mtx_timedlock(&timed, &deadline) == thrd_error;

  /* One timed-out contender must not strand another ordinary contender. */
  atomic_store(&ready, 0);
  valid &= pthread_mutex_lock(&mutex) == 0;
  thrd_t timeout, other;
  if (thrd_create(&timeout, contender, (void *)(uintptr_t)1) != thrd_success ||
      thrd_create(&other, contender, NULL) != thrd_success) {
    _Exit(1);
  }
  wait_ready(2);
  valid &= thrd_join(timeout, &result) == thrd_success && result == ETIMEDOUT;
  valid &= pthread_mutex_destroy(&mutex) == EBUSY;
  valid &= pthread_mutex_unlock(&mutex) == 0;
  valid &= thrd_join(other, &result) == thrd_success && result == 0;
  valid &= increments == 1;

  /* Exercise timeout/unlock races without prescribing which side wins. */
  unsigned expected = increments;
  for (unsigned round = 0; round < 32; ++round) {
    atomic_store(&ready, 0);
    valid &= pthread_mutex_lock(&mutex) == 0;
    if (thrd_create(&timeout, contender, (void *)(uintptr_t)1) != thrd_success ||
        thrd_create(&other, contender, NULL) != thrd_success) {
      _Exit(1);
    }
    wait_ready(2);
    if (round & 1) {
      sleep_ms(2);
    }
    valid &= pthread_mutex_unlock(&mutex) == 0;
    valid &= thrd_join(timeout, &result) == thrd_success;
    valid &= result == 0 || result == ETIMEDOUT;
    expected += result == 0;
    valid &= thrd_join(other, &result) == thrd_success && result == 0;
    ++expected;
  }
  valid &= increments == expected;
  valid &= pthread_mutex_destroy(&mutex) == 0;
  atomic_store(&finished, true);
  valid &= thrd_join(guard, &result) == thrd_success && result == 0;
  printf("threadprobe: runtime-mutex %s; try/owner/recursive/destroy, C11 timed profile, timeout/unlock transfer\n",
      valid ? "passed" : "failed");
  return valid ? 0 : 1;
}
