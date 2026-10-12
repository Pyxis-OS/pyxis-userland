#include <stdlib.h>
#include <stdbool.h>
#include <thread.h>
#include "thread_private.h"
#include "thread_sync.h"

struct thread_key_entry {
  uint32_t generation;
  bool active;
  void (*destructor)(void *);
};

static struct thread_key_entry key_registry[LIBC_THREAD_KEY_LIMIT];
static struct libc_lock key_guard;

static void lock_keys(void)
{
  libc_lock_acquire(&key_guard);
}

static void unlock_keys(void)
{
  libc_lock_release(&key_guard);
}

static bool key_index(libc_thread_key key, size_t *index, uint32_t *generation)
{
  uint32_t slot = (uint32_t)key;
  *generation = key >> 32;
  if (!slot || slot > LIBC_THREAD_KEY_LIMIT || !*generation) {
    return false;
  }
  *index = slot - 1;
  return true;
}

enum call_status libc_thread_key_create(void (*destructor)(void *), libc_thread_key *key)
{
  if (!key) {
    return CALL_BAD_REQUEST;
  }
  *key = 0;
  lock_keys();
  for (size_t index = 0; index < LIBC_THREAD_KEY_LIMIT; index++) {
    struct thread_key_entry *entry = &key_registry[index];
    if (!entry->active && entry->generation != UINT32_MAX) {
      entry->generation++;
      entry->active = true;
      entry->destructor = destructor;
      *key = ((uint64_t)entry->generation << 32) | (index + 1);
      unlock_keys();
      return CALL_OK;
    }
  }
  unlock_keys();
  return CALL_NO_MEMORY;
}

enum call_status libc_thread_key_delete(libc_thread_key key)
{
  size_t index;
  uint32_t generation;
  if (!key_index(key, &index, &generation)) {
    return CALL_BAD_REQUEST;
  }
  lock_keys();
  struct thread_key_entry *entry = &key_registry[index];
  bool valid = entry->active && entry->generation == generation;
  if (valid) {
    entry->active = false;
    entry->destructor = NULL;
  }
  unlock_keys();
  return valid ? CALL_OK : CALL_BAD_REQUEST;
}

enum call_status libc_thread_key_set(libc_thread_key key, void *value)
{
  size_t index;
  uint32_t generation;
  if (!key_index(key, &index, &generation)) {
    return CALL_BAD_REQUEST;
  }
  struct libc_thread_record *record = libc_thread_record();
  lock_keys();
  struct thread_key_entry *entry = &key_registry[index];
  bool valid = entry->active && entry->generation == generation;
  if (valid) {
    record->keys[index] = (struct thread_key_value){generation, value};
  }
  unlock_keys();
  return valid ? CALL_OK : CALL_BAD_REQUEST;
}

enum call_status libc_thread_key_get(libc_thread_key key, void **value)
{
  if (!value) {
    return CALL_BAD_REQUEST;
  }
  *value = NULL;
  size_t index;
  uint32_t generation;
  if (!key_index(key, &index, &generation)) {
    return CALL_BAD_REQUEST;
  }
  struct libc_thread_record *record = libc_thread_record();
  lock_keys();
  struct thread_key_entry *entry = &key_registry[index];
  bool valid = entry->active && entry->generation == generation;
  if (valid && record->keys[index].generation == generation) {
    *value = record->keys[index].value;
  }
  unlock_keys();
  return valid ? CALL_OK : CALL_BAD_REQUEST;
}

/* Itanium ABI module argument is immaterial for one static executable. Native
 * MEMORY backing avoids dependence on gate 8's synchronized allocator. */
int __cxa_thread_atexit_impl(void (*function)(void *), void *argument, void *module)
{
  (void)module;
  struct libc_thread_record *record = libc_thread_record();
  struct thread_destructor_block *block = record->destructors;
  if (block->count == LIBC_THREAD_DESTRUCTOR_BLOCK) {
    struct memory_region region;
    if (memory_allocate(record->memory, sizeof(*block), &region) != CALL_OK) {
      /* Compiler-generated registration ignores the result. Losing a required
       * destructor would falsely report successful TLS initialization. */
      _Exit(EXIT_FAILURE);
    }
    struct thread_destructor_block *next = (void *)(uintptr_t)region.address;
    next->region = region;
    next->previous = block;
    record->destructors = block = next;
  }
  block->handlers[block->count++] = (struct thread_destructor){function, argument};
  return 0;
}

static void run_language_destructors(struct libc_thread_record *record)
{
  for (;;) {
    struct thread_destructor_block *block = record->destructors;
    if (!block->count) {
      if (!block->previous) {
        return;
      }
      record->destructors = block->previous;
      if (memory_release(record->memory, block->region) != CALL_OK) {
        _Exit(EXIT_FAILURE);
      }
      continue;
    }
    struct thread_destructor handler = block->handlers[--block->count];
    handler.function(handler.argument);
  }
}

void libc_thread_destructors(void)
{
  struct libc_thread_record *record = libc_thread_record();
  run_language_destructors(record);
  for (size_t pass = 0; pass < LIBC_THREAD_DESTRUCTOR_PASSES; pass++) {
    bool called = false;
    for (size_t index = 0; index < LIBC_THREAD_KEY_LIMIT; index++) {
      /* Only this thread writes its values. An empty slot needs no registry
       * inspection, including ordinary programs which never use keys. */
      if (!record->keys[index].value) {
        continue;
      }
      lock_keys();
      struct thread_key_entry *entry = &key_registry[index];
      struct thread_key_value *value = &record->keys[index];
      void (*destructor)(void *) = NULL;
      void *argument = NULL;
      if (entry->active && entry->generation == value->generation && value->value &&
          entry->destructor) {
        destructor = entry->destructor;
        argument = value->value;
        value->value = NULL;
      }
      unlock_keys();
      if (destructor) {
        called = true;
        destructor(argument);
      }
    }
    if (!called) {
      break;
    }
  }
  /* A key callback can first initialize a language TLS object. Its destructor
   * still runs before native retirement, after the bounded key passes. */
  run_language_destructors(record);
}

[[noreturn]] void libc_thread_exit(handle_t threads, uint64_t value)
{
  struct libc_thread_record *record = libc_thread_record();
  libc_once_thread_cleanup();
  if (!record->destroying) {
    record->destroying = true;
    libc_thread_destructors();
  }
  libc_once_thread_cleanup();
  thread_exit(threads, value);
  _Exit(EXIT_FAILURE);
}
