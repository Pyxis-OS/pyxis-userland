#ifndef LIBC_SEMAPHORE_H
#define LIBC_SEMAPHORE_H

#include <pyxis/sync.h>
#include <time.h>

typedef struct libc_semaphore sem_t;
#define SEM_VALUE_MAX 2147483647

#ifdef __cplusplus
extern "C" {
#endif

/* Unnamed process-private semaphores only. Timed waits use absolute UTC.
 * Destroy requires caller exclusion and fails while waits remain admitted. */
int sem_init(sem_t *semaphore, int shared, unsigned int value);
int sem_destroy(sem_t *semaphore);
int sem_wait(sem_t *semaphore);
int sem_trywait(sem_t *semaphore);
int sem_timedwait(sem_t *semaphore, const struct timespec *time);
int sem_post(sem_t *semaphore);
int sem_getvalue(sem_t *semaphore, int *value);

#ifdef __cplusplus
}
#endif

#endif
