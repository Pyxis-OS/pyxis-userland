#include <errno.h>
#include <handle.h>
#include <memory.h>
#include <startup.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tlsf.h>
#include "runtime.h"
#include "tlsf_user.h"
#include "thread_sync.h"
#include "memory_operations.h"

#define HEAP_ALIGNMENT 16
#define HEAP_ALIGNMENT_MAX MEMORY_PAGE_SIZE
#define HEAP_CONTROL_BYTES (16 * 1024)
#define HEAP_POOL_BYTES (64 * 1024)
#define HEAP_POOL_PREFIX_BYTES HEAP_ALIGNMENT

static unsigned char control[HEAP_CONTROL_BYTES] __attribute__((aligned(HEAP_ALIGNMENT)));
static tlsf_t allocator;
static struct libc_lock heap_guard;
static handle_t memory = HANDLE_INVALID;

void malloc_init(void)
{
  tlsf_assert(!allocator);
  tlsf_assert(tlsf_size() <= sizeof(control));
  _Static_assert(_Alignof(max_align_t) <= HEAP_ALIGNMENT, "C allocation alignment");
  allocator = tlsf_create(control);
  tlsf_assert(allocator);

  /* Keep allocator authority even if main closes its original startup grant.
   * Missing authority does not prevent a program without a heap from running. */
  handle_copy_restricted(startup_resource("memory"), MEMORY_RIGHT_MANAGE, 0, &memory);
}

static enum call_status allocate_pool(size_t request, struct memory_region *region)
{
  size_t overhead = tlsf_pool_overhead() + HEAP_POOL_PREFIX_BYTES;
  /* Leave space for memalign's leading split and TLSF size-class rounding. */
  size_t slack = request / 16;
  if (request > SIZE_MAX - slack ||
      request + slack > SIZE_MAX - MEMORY_PAGE_SIZE - overhead) {
    return CALL_NO_MEMORY;
  }
  size_t bytes = request + slack + MEMORY_PAGE_SIZE + overhead;
  if (bytes < HEAP_POOL_BYTES) {
    bytes = HEAP_POOL_BYTES;
  }
  if (bytes > SIZE_MAX - (MEMORY_PAGE_SIZE - 1)) {
    return CALL_NO_MEMORY;
  }
  bytes = (bytes + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
  if (bytes - overhead >= tlsf_block_size_max()) {
    return CALL_NO_MEMORY;
  }

  return memory_allocate(memory, bytes, region);
}

static void *allocate(size_t alignment, size_t size)
{
  /* TLSF's internal alignment additions are unchecked. Stay below its top bin,
   * as the kernel wrapper does, including for arbitrary overflowing requests. */
  if (!allocator || memory == HANDLE_INVALID || size > tlsf_block_size_max() / 2) {
    errno = ENOMEM;
    return NULL;
  }
  void *pointer = NULL;
  for (unsigned attempt = 0; attempt < LIBC_VM_ATTEMPTS; attempt++) {
    libc_lock_acquire(&heap_guard);
    pointer = tlsf_memalign(allocator, alignment, size);
    libc_lock_release(&heap_guard);
    if (pointer) {
      return pointer;
    }

    /* Native growth pauses siblings, which may own the heap guard. Recheck
     * existing pools after a BUSY refusal: another grower may have published. */
    struct memory_region region;
    enum call_status status = allocate_pool(size + (alignment - HEAP_ALIGNMENT), &region);
    if (status == CALL_BUSY) {
      continue;
    }
    if (status == CALL_OK) {
      void *pool = (unsigned char *)(uintptr_t)region.address + HEAP_POOL_PREFIX_BYTES;
      libc_lock_acquire(&heap_guard);
      bool added = tlsf_add_pool(allocator, pool,
          region.size - HEAP_POOL_PREFIX_BYTES) != NULL;
      if (added) {
        pointer = tlsf_memalign(allocator, alignment, size);
      }
      libc_lock_release(&heap_guard);
      if (!added) {
        tlsf_assert(libc_memory_release(memory, region) == CALL_OK);
      }
    }
    break;
  }
  if (!pointer) {
    errno = ENOMEM;
  }
  return pointer;
}

void *malloc(size_t size)
{
  if (!size) {
    return NULL;
  }
  return allocate(HEAP_ALIGNMENT, size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
  if (!alignment || (alignment & (alignment - 1)) || alignment > HEAP_ALIGNMENT_MAX) {
    errno = EINVAL;
    return NULL;
  }
  if (!size) {
    return NULL;
  }
  return allocate(alignment < HEAP_ALIGNMENT ? HEAP_ALIGNMENT : alignment, size);
}

void free(void *pointer)
{
  if (pointer) {
    libc_lock_acquire(&heap_guard);
    tlsf_free(allocator, pointer);
    libc_lock_release(&heap_guard);
  }
}

void *calloc(size_t count, size_t size)
{
  if (size && count > SIZE_MAX / size) {
    errno = ENOMEM;
    return NULL;
  }
  size_t bytes = count * size;
  void *pointer = malloc(bytes);
  if (pointer) {
    memset(pointer, 0, bytes);
  }
  return pointer;
}

void *realloc(void *pointer, size_t size)
{
  if (!size) {
    free(pointer);
    return NULL;
  }
  if (!pointer) {
    return malloc(size);
  }
  libc_lock_acquire(&heap_guard);
  size_t capacity = tlsf_block_size(pointer);
  libc_lock_release(&heap_guard);
  if (size <= capacity) {
    return pointer;
  }

  /* TLSF realloc can move to an 8-byte-aligned block. Allocate through our
   * wrapper instead, preserving 16-byte alignment and the old block on failure. */
  void *replacement = malloc(size);
  if (replacement) {
    memcpy(replacement, pointer, capacity);
    free(pointer);
  }
  return replacement;
}
