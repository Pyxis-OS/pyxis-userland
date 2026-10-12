#ifndef LIBC_THREADS_H
#define LIBC_THREADS_H

#include <pyxis/sync.h>
#include <time.h>

typedef uint64_t thrd_t;
typedef uint64_t tss_t;
typedef int (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);
typedef struct libc_mutex mtx_t;
typedef struct libc_condition cnd_t;
typedef struct libc_once once_flag;

enum { thrd_success, thrd_nomem, thrd_timedout, thrd_busy, thrd_error };
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };
#define ONCE_FLAG_INIT LIBC_ONCE_INITIALIZER
#define TSS_DTOR_ITERATIONS 4

#ifdef __cplusplus
extern "C" {
#endif

/* Selected C11 profile: joinable siblings, synchronization and runtime keys.
 * No detach or explicit scheduler-yield operation is advertised. */
int thrd_create(thrd_t *thread, thrd_start_t start, void *argument);
int thrd_join(thrd_t thread, int *result);
thrd_t thrd_current(void);
int thrd_equal(thrd_t left, thrd_t right);
__attribute__((noreturn)) void thrd_exit(int result);
int thrd_sleep(const struct timespec *duration, struct timespec *remaining);
int tss_create(tss_t *key, tss_dtor_t destructor);
void tss_delete(tss_t key);
int tss_set(tss_t key, void *value);
void *tss_get(tss_t key);

int mtx_init(mtx_t *mutex, int type);
void mtx_destroy(mtx_t *mutex);
int mtx_lock(mtx_t *mutex);
int mtx_trylock(mtx_t *mutex);
int mtx_timedlock(mtx_t *mutex, const struct timespec *time);
int mtx_unlock(mtx_t *mutex);
int cnd_init(cnd_t *condition);
void cnd_destroy(cnd_t *condition);
int cnd_wait(cnd_t *condition, mtx_t *mutex);
int cnd_timedwait(cnd_t *condition, mtx_t *mutex, const struct timespec *time);
int cnd_signal(cnd_t *condition);
int cnd_broadcast(cnd_t *condition);
void call_once(once_flag *once, void (*initialize)(void));

#ifdef __cplusplus
}
#endif

#endif
