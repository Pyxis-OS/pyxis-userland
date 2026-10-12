#ifndef LIBC_PTHREAD_H
#define LIBC_PTHREAD_H

#include <pyxis/sync.h>
#include <time.h>

typedef uint64_t pthread_t;
typedef uint64_t pthread_key_t;
typedef struct libc_mutex pthread_mutex_t;
typedef struct libc_condition pthread_cond_t;
typedef struct libc_once pthread_once_t;
typedef struct libc_rwlock pthread_rwlock_t;

typedef struct {
  size_t stack_size;
  int detach_state;
} pthread_attr_t;

typedef struct {
  int type;
  int process_shared;
} pthread_mutexattr_t;

typedef struct {
  clockid_t clock;
  int process_shared;
} pthread_condattr_t;

typedef struct {
  int process_shared;
} pthread_rwlockattr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_MUTEX_NORMAL LIBC_MUTEX_NORMAL
#define PTHREAD_MUTEX_DEFAULT PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_RECURSIVE LIBC_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_INITIALIZER LIBC_MUTEX_INITIALIZER
#define PTHREAD_COND_INITIALIZER LIBC_CONDITION_INITIALIZER
#define PTHREAD_ONCE_INIT LIBC_ONCE_INITIALIZER
#define PTHREAD_RWLOCK_INITIALIZER LIBC_RWLOCK_INITIALIZER
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

#ifdef __cplusplus
extern "C" {
#endif

/* Joinable libc-created siblings only. Initial/native-created threads have
 * identities but no join observer. Detach, cancellation, scheduling policies,
 * robust recovery and process-shared synchronization are outside this profile. */
int pthread_attr_init(pthread_attr_t *attributes);
int pthread_attr_destroy(pthread_attr_t *attributes);
int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *attributes, size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *attributes, int *state);
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*start)(void *), void *argument);
int pthread_join(pthread_t thread, void **result);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t left, pthread_t right);
__attribute__((noreturn)) void pthread_exit(void *result);
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

int pthread_mutexattr_init(pthread_mutexattr_t *attributes);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attributes);
int pthread_mutexattr_settype(pthread_mutexattr_t *attributes, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attributes, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attributes, int shared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attributes, int *shared);
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *time);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_condattr_init(pthread_condattr_t *attributes);
int pthread_condattr_destroy(pthread_condattr_t *attributes);
int pthread_condattr_setclock(pthread_condattr_t *attributes, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *attributes, clockid_t *clock);
int pthread_condattr_setpshared(pthread_condattr_t *attributes, int shared);
int pthread_condattr_getpshared(const pthread_condattr_t *attributes, int *shared);
int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int pthread_cond_destroy(pthread_cond_t *condition);
/* A wait on a recursive mutex requires depth one. Concurrent waiters on one
 * condition must use the same mutex. Every admitted wait reacquires it. */
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
    const struct timespec *time);
int pthread_cond_signal(pthread_cond_t *condition);
int pthread_cond_broadcast(pthread_cond_t *condition);

int pthread_once(pthread_once_t *once, void (*initialize)(void));

/* Process-private reader/writer locks; recursive reader acquisition is allowed.
 * No fairness or reader-to-writer upgrade is promised. */
int pthread_rwlockattr_init(pthread_rwlockattr_t *attributes);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attributes);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attributes, int shared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attributes, int *shared);
int pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attributes);
int pthread_rwlock_destroy(pthread_rwlock_t *lock);
int pthread_rwlock_rdlock(pthread_rwlock_t *lock);
int pthread_rwlock_wrlock(pthread_rwlock_t *lock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *lock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *lock);
int pthread_rwlock_unlock(pthread_rwlock_t *lock);

#ifdef __cplusplus
}
#endif

#endif
