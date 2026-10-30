#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>

#define RUNTIME_ROUNDS 128
#define RUNTIME_WORKERS 2

struct runtime_worker {
  thrd_t identity;
  uintptr_t fs, stack, tls, error;
  void *allocation;
  unsigned entered, finished;
  int valid;
};

/* Ordinary shared storage: inspected with QEMU's debugger in runtime-inspect. */
struct runtime_worker threadprobe_runtime_workers[RUNTIME_WORKERS];
pthread_mutex_t threadprobe_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t threadprobe_runtime_condition = PTHREAD_COND_INITIALIZER;
sem_t threadprobe_runtime_semaphore;
static unsigned runtime_release, runtime_count;
static unsigned runtime_wait_seconds = 5;
static _Thread_local unsigned runtime_tls = 0x6178;
static once_flag runtime_once = ONCE_FLAG_INIT;
static unsigned runtime_once_calls;

static bool future(clockid_t clock, long nanoseconds, struct timespec *deadline)
{
  if (clock_gettime(clock, deadline)) {
    return false;
  }
  deadline->tv_nsec += nanoseconds;
  if (deadline->tv_nsec >= 1000000000) {
    deadline->tv_nsec -= 1000000000;
    deadline->tv_sec++;
  }
  return true;
}

static void initialize_once(void)
{
  runtime_once_calls++;
}

static int runtime_worker(void *argument)
{
  struct runtime_worker *worker = argument;
  unsigned index = worker - threadprobe_runtime_workers;
  uintptr_t stack_marker;
  worker->identity = thrd_current();
  __asm__ volatile("mov %%fs:0, %0" : "=r"(worker->fs));
  worker->stack = (uintptr_t)&stack_marker;
  worker->tls = (uintptr_t)&runtime_tls;
  worker->error = (uintptr_t)&errno;
  worker->valid = runtime_tls == 0x6178;
  runtime_tls = index + 1;
  errno = index ? ERANGE : EBUSY;
  call_once(&runtime_once, initialize_once);
  if (pthread_mutex_lock(&threadprobe_runtime_mutex)) {
    return 1;
  }
  worker->entered = 1;
  pthread_cond_broadcast(&threadprobe_runtime_condition);
  struct timespec deadline;
  if (!future(CLOCK_REALTIME, 0, &deadline)) {
    pthread_mutex_unlock(&threadprobe_runtime_mutex);
    return 2;
  }
  deadline.tv_sec += runtime_wait_seconds;
  while (!runtime_release) {
    if (pthread_cond_timedwait(&threadprobe_runtime_condition,
        &threadprobe_runtime_mutex, &deadline)) {
      pthread_mutex_unlock(&threadprobe_runtime_mutex);
      return 3;
    }
  }
  pthread_mutex_unlock(&threadprobe_runtime_mutex);
  for (unsigned round = 0; round < RUNTIME_ROUNDS; round++) {
    if (pthread_mutex_lock(&threadprobe_runtime_mutex)) {
      return 4;
    }
    runtime_count++;
    pthread_mutex_unlock(&threadprobe_runtime_mutex);
    void *allocation = malloc(64 + index * 8192);
    if (!allocation) {
      return 5;
    }
    free(allocation);
  }
  worker->allocation = malloc(1024 + index * 16384);
  worker->valid &= runtime_tls == index + 1 && errno == (index ? ERANGE : EBUSY);
  if (sem_post(&threadprobe_runtime_semaphore)) {
    return 6;
  }
  __atomic_store_n(&worker->finished, 1, __ATOMIC_RELEASE);
  return worker->valid && worker->allocation ? (int)index + 17 : 7;
}

__attribute__((noinline)) void threadprobe_runtime_checkpoint(void)
{
  __asm__ volatile("" ::: "memory");
}

int threadprobe_runtime(bool inspect)
{
  runtime_wait_seconds = inspect ? 300 : 5;
  thrd_t workers[RUNTIME_WORKERS];
  bool valid = thrd_current() != 0 && thrd_equal(thrd_current(), pthread_self());
  errno = EINVAL;
  valid &= runtime_tls == 0x6178;
  if (sem_init(&threadprobe_runtime_semaphore, 0, 0)) {
    return 1;
  }
  for (unsigned index = 0; index < RUNTIME_WORKERS; index++) {
    if (thrd_create(&workers[index], runtime_worker, &threadprobe_runtime_workers[index]) !=
        thrd_success) {
      _Exit(1);
    }
  }
  pthread_mutex_lock(&threadprobe_runtime_mutex);
  struct timespec deadline;
  if (!future(CLOCK_REALTIME, 0, &deadline)) {
    _Exit(1);
  }
  deadline.tv_sec += runtime_wait_seconds;
  while (!threadprobe_runtime_workers[0].entered || !threadprobe_runtime_workers[1].entered) {
    if (pthread_cond_timedwait(&threadprobe_runtime_condition,
        &threadprobe_runtime_mutex, &deadline)) {
      _Exit(1);
    }
  }
  valid &= pthread_cond_destroy(&threadprobe_runtime_condition) == EBUSY;
  valid &= pthread_mutex_destroy(&threadprobe_runtime_mutex) == EBUSY;
  pthread_mutex_unlock(&threadprobe_runtime_mutex);
  threadprobe_runtime_checkpoint();
  if (inspect) {
    puts("threadprobe: runtime siblings at condition; inspect FS/CR3/stacks, registry, parking and leases, then Enter");
    fflush(stdout);
    (void)getchar();
  }
  pthread_mutex_lock(&threadprobe_runtime_mutex);
  runtime_release = 1;
  valid &= pthread_cond_broadcast(&threadprobe_runtime_condition) == 0;
  pthread_mutex_unlock(&threadprobe_runtime_mutex);
  for (unsigned index = 0; index < RUNTIME_WORKERS; index++) {
    if (!future(CLOCK_REALTIME, 0, &deadline)) {
      _Exit(1);
    }
    deadline.tv_sec += 5;
    valid &= sem_timedwait(&threadprobe_runtime_semaphore, &deadline) == 0;
    int result = 0;
    valid &= thrd_join(workers[index], &result) == thrd_success && result == (int)index + 17;
    valid &= thrd_join(workers[index], NULL) == thrd_error;
    free(threadprobe_runtime_workers[index].allocation);
  }
  struct runtime_worker *first = &threadprobe_runtime_workers[0];
  struct runtime_worker *second = &threadprobe_runtime_workers[1];
  valid &= first->fs != second->fs && first->tls != second->tls &&
      first->error != second->error && first->stack != second->stack &&
      first->identity != second->identity && first->fs && second->fs;
  valid &= runtime_count == RUNTIME_ROUNDS * RUNTIME_WORKERS && runtime_once_calls == 1;
  valid &= runtime_tls == 0x6178 && errno == EINVAL;
  valid &= sem_destroy(&threadprobe_runtime_semaphore) == 0;
  valid &= pthread_cond_destroy(&threadprobe_runtime_condition) == 0;
  valid &= pthread_mutex_destroy(&threadprobe_runtime_mutex) == 0;
  threadprobe_runtime_checkpoint();
  printf("threadprobe: runtime %s; distinct stack/FS/TLS/errno, %u increments, once=%u, join and cross-thread free\n",
      valid ? "passed" : "failed", runtime_count, runtime_once_calls);
  return valid ? 0 : 1;
}

static pthread_mutex_t timed_mutex = PTHREAD_MUTEX_INITIALIZER;
static int timed_worker(void *argument)
{
  (void)argument;
  struct timespec deadline;
  if (!future(CLOCK_REALTIME, 20000000, &deadline)) {
    return 1;
  }
  return pthread_mutex_timedlock(&timed_mutex, &deadline) == ETIMEDOUT ? 0 : 2;
}

int threadprobe_runtime_time(void)
{
  bool valid = pthread_mutex_lock(&timed_mutex) == 0;
  thrd_t worker;
  if (thrd_create(&worker, timed_worker, NULL) != thrd_success) {
    _Exit(1);
  }
  int result;
  valid &= thrd_join(worker, &result) == thrd_success && result == 0;
  valid &= pthread_mutex_unlock(&timed_mutex) == 0;
  pthread_condattr_t attributes;
  pthread_cond_t condition;
  valid &= pthread_condattr_init(&attributes) == 0;
  valid &= pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0;
  valid &= pthread_cond_init(&condition, &attributes) == 0;
  valid &= pthread_mutex_lock(&timed_mutex) == 0;
  struct timespec deadline;
  if (!future(CLOCK_MONOTONIC, 20000000, &deadline)) {
    _Exit(1);
  }
  valid &= pthread_cond_timedwait(&condition, &timed_mutex, &deadline) == ETIMEDOUT;
  /* A successful unlock proves the timed condition wait reacquired ownership. */
  valid &= pthread_mutex_unlock(&timed_mutex) == 0;
  valid &= pthread_cond_destroy(&condition) == 0;
  valid &= pthread_condattr_destroy(&attributes) == 0;
  pthread_mutexattr_t mutex_attributes;
  valid &= pthread_mutexattr_init(&mutex_attributes) == 0;
  valid &= pthread_mutexattr_settype(&mutex_attributes, PTHREAD_MUTEX_RECURSIVE) == 0;
  pthread_mutex_t recursive;
  valid &= pthread_mutex_init(&recursive, &mutex_attributes) == 0;
  valid &= pthread_mutex_lock(&recursive) == 0 && pthread_mutex_lock(&recursive) == 0;
  valid &= pthread_mutex_unlock(&recursive) == 0 && pthread_mutex_unlock(&recursive) == 0;
  valid &= pthread_mutex_destroy(&recursive) == 0;
  valid &= pthread_mutexattr_setpshared(&mutex_attributes, PTHREAD_PROCESS_SHARED) == ENOTSUP;
  valid &= pthread_mutexattr_settype(&mutex_attributes, PTHREAD_MUTEX_ERRORCHECK) == ENOTSUP;
  pthread_attr_t thread_attributes;
  valid &= pthread_attr_init(&thread_attributes) == 0;
  valid &= pthread_attr_setstacksize(&thread_attributes, 1024 * 1024 + 1) == EINVAL;
  valid &= pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_DETACHED) == 0;
  int detach_state;
  valid &= pthread_attr_getdetachstate(&thread_attributes, &detach_state) == 0 &&
      detach_state == PTHREAD_CREATE_DETACHED;
  valid &= pthread_attr_setdetachstate(&thread_attributes, PTHREAD_CREATE_JOINABLE) == 0;
  valid &= pthread_detach(pthread_self()) == EINVAL;
  valid &= pthread_join(pthread_self(), NULL) == EDEADLK;
  valid &= pthread_join(UINT64_MAX, NULL) == ESRCH;
  sem_t semaphore;
  valid &= sem_init(&semaphore, 1, 0) == -1 && errno == ENOTSUP;
  valid &= sem_init(&semaphore, 0, SEM_VALUE_MAX) == 0;
  valid &= sem_post(&semaphore) == -1 && errno == EOVERFLOW;
  valid &= sem_destroy(&semaphore) == 0;
  valid &= sem_init(&semaphore, 0, 0) == 0;
  valid &= sem_trywait(&semaphore) == -1 && errno == EAGAIN;
  if (!future(CLOCK_REALTIME, 20000000, &deadline)) {
    _Exit(1);
  }
  valid &= sem_timedwait(&semaphore, &deadline) == -1 && errno == ETIMEDOUT;
  valid &= sem_destroy(&semaphore) == 0;
  printf("threadprobe: runtime-time %s; UTC mutex/semaphore, monotonic condition reacquire, recursive, detach and unsupported profiles\n",
      valid ? "passed" : "failed");
  return valid ? 0 : 1;
}

static thrd_t limit_threads[64];
static uint32_t limit_release;
static int limit_worker(void *argument)
{
  (void)argument;
  while (!__atomic_load_n(&limit_release, __ATOMIC_ACQUIRE)) {
    struct timespec duration = {.tv_nsec = 1000000};
    if (thrd_sleep(&duration, NULL)) {
      return 1;
    }
  }
  return 0;
}

int threadprobe_runtime_limit(void)
{
  unsigned count = 0;
  int creation = thrd_success;
  while (count < sizeof(limit_threads) / sizeof(limit_threads[0]) &&
      (creation = thrd_create(&limit_threads[count], limit_worker, NULL)) == thrd_success) {
    count++;
  }
  /* This mode is qualified with 2 GiB: a real allocation failure is separately
   * exercised by exhaust. The bound is implementation tuning, not an ABI. */
  bool valid = count == 63 && creation == thrd_error;
  __atomic_store_n(&limit_release, 1, __ATOMIC_RELEASE);
  for (unsigned index = 0; index < count; index++) {
    int result;
    valid &= thrd_join(limit_threads[index], &result) == thrd_success && result == 0;
  }
  thrd_t reused;
  int result;
  valid &= thrd_create(&reused, limit_worker, NULL) == thrd_success;
  valid &= thrd_join(reused, &result) == thrd_success && result == 0;
  threadprobe_runtime_checkpoint();
  printf("threadprobe: runtime-limit %s; %u siblings, refusal=%d, all collected and generation reused\n",
      valid ? "passed" : "failed", count, creation);
  return valid ? 0 : 1;
}

static pthread_once_t exit_once = PTHREAD_ONCE_INIT;
static unsigned exit_once_attempts;
static void exit_initializer(void)
{
  if (++exit_once_attempts == 1) {
    pthread_exit((void *)(uintptr_t)0x731);
  }
}

static void *once_worker(void *argument)
{
  (void)argument;
  return (void *)(uintptr_t)pthread_once(&exit_once, exit_initializer);
}

int threadprobe_runtime_once(void)
{
  pthread_t worker;
  void *result;
  bool valid = true;
  for (unsigned round = 0; round < 2; round++) {
    if (pthread_create(&worker, NULL, once_worker, NULL)) {
      _Exit(1);
    }
    if (pthread_join(worker, &result)) {
      _Exit(1);
    }
    valid &= result == (round ? NULL : (void *)(uintptr_t)0x731);
  }
  valid &= exit_once_attempts == 2 && exit_once.state == 2 && !exit_once.waiters;
  threadprobe_runtime_checkpoint();
  printf("threadprobe: runtime-once %s; initializer exit reset, second attempt published, both joined\n",
      valid ? "passed" : "failed");
  return valid ? 0 : 1;
}
