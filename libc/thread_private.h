#ifndef LIBC_THREAD_PRIVATE_H
#define LIBC_THREAD_PRIVATE_H

#include <memory.h>
#include <pyxis/thread_runtime.h>

struct startup_info;

/* Userland implementation tuning, shared by registry and per-thread storage. */
#ifndef LIBC_THREAD_KEY_LIMIT
#define LIBC_THREAD_KEY_LIMIT 128
#endif
#define LIBC_THREAD_DESTRUCTOR_PASSES 4
#define LIBC_THREAD_DESTRUCTOR_BLOCK 32

struct thread_key_value {
  uint32_t generation;
  void *value;
};

struct thread_destructor {
  void (*function)(void *);
  void *argument;
};

struct thread_destructor_block {
  struct thread_destructor_block *previous;
  struct memory_region region;
  size_t count;
  struct thread_destructor handlers[LIBC_THREAD_DESTRUCTOR_BLOCK];
};

struct libc_thread_record {
  struct libc_thread_record *self;
  int errno_value;
  handle_t memory;
  struct thread_destructor_block *destructors;
  struct thread_destructor_block first_destructors;
  struct thread_key_value keys[LIBC_THREAD_KEY_LIMIT];
};

_Static_assert(offsetof(struct libc_thread_record, self) == 0, "FS self pointer");
_Static_assert(LIBC_THREAD_KEY_LIMIT > 0 && LIBC_THREAD_KEY_LIMIT <= UINT32_MAX,
    "runtime key index capacity");

static inline struct libc_thread_record *libc_thread_record(void)
{
  struct libc_thread_record *record;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(record));
  return record;
}

void libc_thread_bootstrap(const struct startup_info *info);
void libc_thread_destructors(void);

#endif
