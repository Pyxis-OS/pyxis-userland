#include "memory_operations.h"
#include <stdlib.h>
#include <handle.h>
#include <string.h>
#include <stdbool.h>
#include <thread.h>
#include "thread_private.h"
#include "signal_internal.h"
#include "thread_sync.h"

struct thread_key_entry {
  uint32_t generation;
  uint32_t next_free;
  bool active;
  void (*destructor)(void *);
};

struct thread_key_registry {
  struct memory_region region;
  handle_t memory;
  size_t capacity;
};

/* Each published table retains its own MEMORY handle, independent of the TCB
 * which created it. Only key_guard protects the movable table and free list. */
static struct thread_key_registry key_registry;
static uint32_t key_free_slot;
static struct libc_lock key_guard;

static void lock_keys(void)
{
  libc_lock_acquire(&key_guard);
}

static void unlock_keys(void)
{
  libc_lock_release(&key_guard);
}

static struct thread_key_entry *key_entries(void)
{
  return (void *)(uintptr_t)key_registry.region.address;
}

static bool key_index(libc_thread_key key, size_t *index, uint32_t *generation)
{
  uint32_t slot = (uint32_t)key;
  *generation = key >> 32;
  if (!slot || !*generation) {
    return false;
  }
  *index = slot - 1;
  return true;
}

/* The native page is a starting allocation, not a key-count admission limit. */
static size_t key_capacity(size_t old, size_t required, size_t element_size)
{
  size_t maximum = SIZE_MAX / element_size;
  if (maximum > UINT32_MAX) {
    maximum = UINT32_MAX;
  }
  size_t capacity = old ? old : MEMORY_PAGE_SIZE / element_size;
  if (required > maximum) {
    return 0;
  }
  while (capacity < required) {
    capacity = capacity > maximum / 2 ? maximum : capacity * 2;
  }
  return capacity;
}

static void release_key_registry(struct thread_key_registry storage)
{
  if (!storage.capacity) {
    return;
  }
  /* A failed release cannot be reported as a failed create after publication.
   * Match other runtime ownership cleanup: uncertainty stops the process. */
  if (libc_memory_release(storage.memory, storage.region) != CALL_OK ||
      handle_close(storage.memory) != 0) {
    _Exit(EXIT_FAILURE);
  }
}

static libc_thread_key take_key(void (*destructor)(void *))
{
  uint32_t slot = key_free_slot;
  struct thread_key_entry *entry = &key_entries()[slot - 1];
  key_free_slot = entry->next_free;
  entry->generation++;
  entry->active = true;
  entry->destructor = destructor;
  return ((uint64_t)entry->generation << 32) | slot;
}

enum call_status libc_thread_key_create(void (*destructor)(void *), libc_thread_key *key)
{
  if (!key) {
    return CALL_BAD_REQUEST;
  }
  *key = 0;
  for (;;) {
    lock_keys();
    if (key_free_slot) {
      *key = take_key(destructor);
      unlock_keys();
      return CALL_OK;
    }
    size_t previous_capacity = key_registry.capacity;
    unlock_keys();
    size_t capacity = key_capacity(previous_capacity, previous_capacity + 1,
        sizeof(struct thread_key_entry));
    if (!capacity) {
      return CALL_LIMIT;
    }
    struct thread_key_registry candidate = {.capacity = capacity};
    enum call_status status = handle_copy_restricted(libc_thread_record()->memory,
        MEMORY_RIGHT_MANAGE, 0, &candidate.memory);
    if (status != CALL_OK) {
      return status;
    }
    status = libc_memory_allocate(candidate.memory,
        capacity * sizeof(struct thread_key_entry), &candidate.region);
    if (status != CALL_OK) {
      if (handle_close(candidate.memory) != 0) {
        _Exit(EXIT_FAILURE);
      }
      return status;
    }
    lock_keys();
    if (key_free_slot || key_registry.capacity != previous_capacity) {
      unlock_keys();
      release_key_registry(candidate);
      continue;
    }
    struct thread_key_entry *entries = (void *)(uintptr_t)candidate.region.address;
    if (previous_capacity) {
      memcpy(entries, key_entries(), previous_capacity * sizeof(*entries));
    }
    for (size_t index = previous_capacity; index < capacity; index++) {
      entries[index].next_free = index + 1 < capacity ? index + 2 : 0;
    }
    struct thread_key_registry previous = key_registry;
    key_registry = candidate;
    key_free_slot = previous_capacity + 1;
    *key = take_key(destructor);
    unlock_keys();
    release_key_registry(previous);
    return CALL_OK;
  }
}

static bool key_valid(size_t index, uint32_t generation)
{
  return index < key_registry.capacity && key_entries()[index].active &&
      key_entries()[index].generation == generation;
}

enum call_status libc_thread_key_delete(libc_thread_key key)
{
  size_t index;
  uint32_t generation;
  if (!key_index(key, &index, &generation)) {
    return CALL_BAD_REQUEST;
  }
  lock_keys();
  bool valid = key_valid(index, generation);
  if (valid) {
    struct thread_key_entry *entry = &key_entries()[index];
    entry->active = false;
    entry->destructor = NULL;
    if (generation != UINT32_MAX) {
      entry->next_free = key_free_slot;
      key_free_slot = index + 1;
    }
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
  bool valid = key_valid(index, generation);
  if (!valid || index < record->key_capacity || !value) {
    if (valid && index < record->key_capacity) {
      record->keys[index] = (struct thread_key_value){.generation = generation,
          .value = value};
    }
    unlock_keys();
    return valid ? CALL_OK : CALL_BAD_REQUEST;
  }
  unlock_keys();
  size_t capacity = key_capacity(record->key_capacity, index + 1,
      sizeof(struct thread_key_value));
  if (!capacity) {
    return CALL_LIMIT;
  }
  struct memory_region region;
  enum call_status status = libc_memory_allocate(record->memory,
      capacity * sizeof(struct thread_key_value), &region);
  if (status != CALL_OK) {
    return status;
  }
  struct thread_key_value *values = (void *)(uintptr_t)region.address;
  lock_keys();
  valid = key_valid(index, generation);
  struct memory_region previous = record->key_region;
  if (valid) {
    if (record->key_capacity) {
      memcpy(values, record->keys, record->key_capacity * sizeof(*values));
    }
    values[index] = (struct thread_key_value){.generation = generation, .value = value};
    record->keys = values;
    record->key_region = region;
    record->key_capacity = capacity;
  }
  unlock_keys();
  struct memory_region unused = valid ? previous : region;
  if (unused.size && libc_memory_release(record->memory, unused) != CALL_OK) {
    _Exit(EXIT_FAILURE);
  }
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
  bool valid = key_valid(index, generation);
  if (valid && index < record->key_capacity &&
      record->keys[index].generation == generation) {
    *value = record->keys[index].value;
  }
  unlock_keys();
  return valid ? CALL_OK : CALL_BAD_REQUEST;
}

enum call_status libc_thread_cleanup_register(void (*cleanup)(void))
{
  struct libc_thread_record *record = libc_thread_record();
  if (!cleanup || record->cleanup_started) {
    return CALL_BAD_REQUEST;
  }
  if (record->cleanup && record->cleanup != cleanup) {
    return CALL_ALREADY_EXISTS;
  }
  record->cleanup = cleanup;
  return CALL_OK;
}

/* Itanium ABI module argument is immaterial for one static executable. Native
 * MEMORY backing avoids dependence on gate 8's synchronized allocator. */
int __cxa_thread_atexit_impl(void (*function)(void *), void *argument, void *module)
{
  (void)module;
  struct libc_thread_record *record = libc_thread_record();
  if (record->cleanup_started && record->cleanup) {
    /* Runtime cleanup has run; generated callers ignore registration failure. */
    _Exit(EXIT_FAILURE);
  }
  struct thread_destructor_block *block = record->destructors;
  if (block->count == LIBC_THREAD_DESTRUCTOR_BLOCK) {
    struct memory_region region;
    if (libc_memory_allocate(record->memory, sizeof(*block), &region) != CALL_OK) {
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
      if (libc_memory_release(record->memory, block->region) != CALL_OK) {
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
  libc_signal_retire();
  struct libc_thread_record *record = libc_thread_record();
  run_language_destructors(record);
  for (size_t pass = 0; pass < LIBC_THREAD_DESTRUCTOR_PASSES; pass++) {
    bool called = false;
    /* Snapshot eligibility before callbacks. Setting a value resets pending,
     * so slots added or rearmed by callbacks wait until a later finite pass. */
    size_t capacity = record->key_capacity;
    for (size_t index = 0; index < capacity; index++) {
      record->keys[index].pending = record->keys[index].value != NULL;
    }
    for (size_t index = 0; index < capacity; index++) {
      if (!record->keys[index].pending) {
        continue;
      }
      void (*destructor)(void *) = NULL;
      void *argument = NULL;
      {
        lock_keys();
        struct thread_key_value *value = &record->keys[index];
        value->pending = false;
        if (key_valid(index, value->generation) && value->value &&
            key_entries()[index].destructor) {
          destructor = key_entries()[index].destructor;
          argument = value->value;
          value->value = NULL;
        }
        unlock_keys();
      }
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
  record->cleanup_started = true;
  if (record->cleanup) {
    void (*cleanup)(void) = record->cleanup;
    /* Retain the opt-in identity for language registration after cleanup. */
    cleanup();
  }
  if (record->key_region.size) {
    struct memory_region region = record->key_region;
    record->keys = NULL;
    record->key_capacity = 0;
    record->key_region = (struct memory_region){0};
    if (libc_memory_release(record->memory, region) != CALL_OK) {
      _Exit(EXIT_FAILURE);
    }
  }
}

[[noreturn]] void libc_thread_exit(handle_t threads, uint64_t value)
{
  struct libc_thread_record *record = libc_thread_record();
  libc_once_thread_cleanup();
  /* A recursive exit cannot claim normal completion while abandoning later
   * language/key cleanup (which may own exit-notification locks). */
  if (record->destroying) {
    _Exit(EXIT_FAILURE);
  }
  record->destroying = true;
  libc_thread_destructors();
  libc_once_thread_cleanup();
  thread_exit(threads, value);
  _Exit(EXIT_FAILURE);
}
