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

#define HEAP_ALIGNMENT 16
#define HEAP_CONTROL_BYTES (16 * 1024)
#define HEAP_POOL_BYTES (64 * 1024)
#define HEAP_POOL_PREFIX_BYTES HEAP_ALIGNMENT

static unsigned char control[HEAP_CONTROL_BYTES] __attribute__((aligned(HEAP_ALIGNMENT)));
static tlsf_t allocator;
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
  handle_copy_restricted(startup_resource("memory"), MEMORY_RIGHT_MANAGE, &memory);
}

static bool add_pool(size_t request)
{
  size_t overhead = tlsf_pool_overhead() + HEAP_POOL_PREFIX_BYTES;
  /* Leave space for memalign's leading split and TLSF size-class rounding. */
  size_t slack = request / 16;
  if (request > SIZE_MAX - slack ||
      request + slack > SIZE_MAX - MEMORY_PAGE_SIZE - overhead) {
    return false;
  }
  size_t bytes = request + slack + MEMORY_PAGE_SIZE + overhead;
  if (bytes < HEAP_POOL_BYTES) {
    bytes = HEAP_POOL_BYTES;
  }
  if (bytes > SIZE_MAX - (MEMORY_PAGE_SIZE - 1)) {
    return false;
  }
  bytes = (bytes + MEMORY_PAGE_SIZE - 1) & ~(MEMORY_PAGE_SIZE - 1);
  if (bytes - overhead >= tlsf_block_size_max()) {
    return false;
  }

  struct memory_region region;
  if (memory_allocate(memory, bytes, &region) != CALL_OK) {
    return false;
  }
  /* TLSF forms a header one word before the pool; keep it inside our mapping. */
  void *pool = (unsigned char *)(uintptr_t)region.address + HEAP_POOL_PREFIX_BYTES;
  if (!tlsf_add_pool(allocator, pool, region.size - HEAP_POOL_PREFIX_BYTES)) {
    tlsf_assert(memory_release(memory, region) == CALL_OK);
    return false;
  }
  return true;
}

void *malloc(size_t size)
{
  if (!size) {
    return NULL;
  }
  /* TLSF's internal alignment additions are unchecked. Stay below its top bin,
   * as the kernel wrapper does, including for arbitrary overflowing requests. */
  if (!allocator || memory == HANDLE_INVALID || size > tlsf_block_size_max() / 2) {
    errno = ENOMEM;
    return NULL;
  }
  void *pointer = tlsf_memalign(allocator, HEAP_ALIGNMENT, size);
  if (!pointer && add_pool(size)) {
    pointer = tlsf_memalign(allocator, HEAP_ALIGNMENT, size);
  }
  if (!pointer) {
    errno = ENOMEM;
  }
  return pointer;
}

void free(void *pointer)
{
  if (pointer) {
    tlsf_free(allocator, pointer);
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
  size_t capacity = tlsf_block_size(pointer);
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
