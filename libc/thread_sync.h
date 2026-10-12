#ifndef LIBC_THREAD_SYNC_H
#define LIBC_THREAD_SYNC_H

#include <pyxis/sync.h>
#include <time.h>
#include <stdbool.h>

/* Internal guards bootstrap without allocation. Native failures cannot leave
 * callers operating on unprotected libc metadata: these void helpers stop. */
void libc_lock_acquire(struct libc_lock *lock);
void libc_lock_release(struct libc_lock *lock);
int libc_park(uint32_t *word, uint32_t expected, uint64_t deadline);
int libc_wake(uint32_t *word, uint64_t maximum);

int libc_sync_validate_time(const struct timespec *time, clockid_t clock);
int libc_sync_wait(uint32_t *word, uint32_t expected,
    const struct timespec *time, clockid_t clock);
int libc_mutex_timedlock(struct libc_mutex *mutex, const struct timespec *time,
    clockid_t clock);
int libc_condition_init(struct libc_condition *condition, clockid_t clock);
int libc_condition_destroy(struct libc_condition *condition);
int libc_condition_wait(struct libc_condition *condition, struct libc_mutex *mutex,
    const struct timespec *time);
int libc_condition_signal(struct libc_condition *condition, bool all);

int libc_once_run(struct libc_once *once, void (*initialize)(void));
void libc_once_thread_cleanup(void);

int libc_rwlock_init(struct libc_rwlock *lock);
int libc_rwlock_destroy(struct libc_rwlock *lock);
int libc_rwlock_lock(struct libc_rwlock *lock, bool write, bool try_only);
int libc_rwlock_unlock(struct libc_rwlock *lock);

#endif
