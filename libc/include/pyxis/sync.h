#ifndef LIBC_PYXIS_SYNC_H
#define LIBC_PYXIS_SYNC_H

#include <stdint.h>
#include <stddef.h>

/* Process-private, in-place objects. Callers exclude new operations before
 * destruction and keep storage alive until every admitted operation returns. */
struct libc_lock {
  /* Zero is free, one held without contention, two held with possible waiters. */
  uint32_t word;
};

#define LIBC_LOCK_INITIALIZER {0}
#define LIBC_MUTEX_NORMAL 0
#define LIBC_MUTEX_RECURSIVE 1
#define LIBC_MUTEX_TIMED 2

struct libc_mutex {
  struct libc_lock guard;
  uint32_t held;
  uint32_t waiters;
  uint32_t depth;
  uint32_t type;
  uint64_t owner;
};

#define LIBC_MUTEX_INITIALIZER {{0}, 0, 0, 0, LIBC_MUTEX_NORMAL, 0}
#define LIBC_RECURSIVE_MUTEX_INITIALIZER {{0}, 0, 0, 0, LIBC_MUTEX_RECURSIVE, 0}

struct libc_condition {
  struct libc_lock guard;
  uintptr_t first;
  uintptr_t last;
  uintptr_t mutex;
  uint32_t waiters;
  int clock;
};

/* CLOCK_REALTIME is zero. */
#define LIBC_CONDITION_INITIALIZER {{0}, 0, 0, 0, 0, 0}

struct libc_once {
  struct libc_lock guard;
  /* Zero is uninitialized, one initializing, two completely published. */
  uint32_t state;
  uint32_t waiters;
};

#define LIBC_ONCE_INITIALIZER {{0}, 0, 0}

struct libc_once_frame {
  struct libc_once_frame *previous;
  struct libc_once *object;
};

struct libc_rwlock {
  struct libc_lock guard;
  uint32_t state;
  uint32_t waiters;
  uint32_t valid;
  uint64_t owner;
};

#define LIBC_RWLOCK_INITIALIZER {{0}, 0, 0, 1, 0}

struct libc_semaphore {
  struct libc_lock guard;
  uint32_t count;
  uint32_t waiters;
  uint32_t valid;
};

#ifdef __cplusplus
extern "C" {
#endif

int libc_mutex_init(struct libc_mutex *mutex, int type);
int libc_mutex_destroy(struct libc_mutex *mutex);
int libc_mutex_lock(struct libc_mutex *mutex);
int libc_mutex_trylock(struct libc_mutex *mutex);
int libc_mutex_unlock(struct libc_mutex *mutex);

/* A C++ adapter keeps the frame on its own stack so an exception can abort
 * initialization without unwinding through a C callback frame. Begin sets run
 * to one only for the initializer; that caller must complete or abort once.
 * Ordinary thread exit aborts outstanding frames through the TCB chain. */
int libc_once_begin(struct libc_once *once, struct libc_once_frame *frame, int *run);
void libc_once_complete(struct libc_once_frame *frame);
void libc_once_abort(struct libc_once_frame *frame);

#ifdef __cplusplus
}
#endif

#endif
