#include "memory_operations.h"
#include "runtime.h"
#include <handle.h>
#include <environment.h>
#include <startup.h>
#include <stdlib.h>
#include <thread.h>
#include "thread_private.h"

/* This translation unit and its native helpers must remain TLS-free: the
 * initial task enters with FS=0 and cannot use errno or malloc here. */
extern const unsigned char __tls_template_start[];
extern const unsigned char __tls_template_init_end[];
extern const unsigned char __tls_template_end[];
extern const unsigned char __tls_alignment[];

static bool add_size(size_t left, size_t right, size_t *result)
{
  if (right > SIZE_MAX - left) {
    return false;
  }
  *result = left + right;
  return true;
}

enum call_status libc_thread_context_prepare(handle_t memory,
    struct libc_thread_context *context)
{
  if (!context) {
    return CALL_BAD_REQUEST;
  }
  *context = (struct libc_thread_context){0};
  uintptr_t start = (uintptr_t)__tls_template_start;
  uintptr_t initialized_end = (uintptr_t)__tls_template_init_end;
  uintptr_t end = (uintptr_t)__tls_template_end;
  size_t tls_alignment = (uintptr_t)__tls_alignment;
  if (initialized_end < start || end < initialized_end || !tls_alignment ||
      (tls_alignment & (tls_alignment - 1))) {
    return CALL_BAD_REQUEST;
  }
  size_t alignment = tls_alignment;
  if (alignment < _Alignof(struct libc_thread_record)) {
    alignment = _Alignof(struct libc_thread_record);
  }
  size_t initialized = initialized_end - start;
  size_t bytes = end - start;
  size_t rounded, usable;
  if (!add_size(bytes, tls_alignment - 1, &rounded)) {
    return CALL_BAD_REQUEST;
  }
  rounded &= ~(tls_alignment - 1);
  if (!add_size(rounded, sizeof(struct libc_thread_record), &usable) ||
      !add_size(usable, alignment - 1, &usable)) {
    return CALL_BAD_REQUEST;
  }
  handle_t owned_memory;
  enum call_status status = handle_copy_restricted(memory, MEMORY_RIGHT_MANAGE, 0,
      &owned_memory);
  if (status != CALL_OK) {
    return status;
  }
  struct memory_reservation reservation;
  status = libc_memory_allocate_guarded(owned_memory, usable, &reservation);
  if (status != CALL_OK) {
    handle_close(owned_memory);
    return status;
  }
  uintptr_t usable_start = reservation.address + MEMORY_PAGE_SIZE;
  uintptr_t reservation_end = reservation.address + reservation.size;
  if (rounded > reservation_end - usable_start ||
      alignment - 1 > UINTPTR_MAX - usable_start - rounded) {
    libc_memory_release_reservation(owned_memory, reservation.identity);
    handle_close(owned_memory);
    return CALL_BAD_REQUEST;
  }
  /* TPOFF is relative to the TLS block rounded to its ELF alignment, even when
   * the libc record needs stricter alignment. Align FS, then locate the image. */
  uintptr_t fs = (usable_start + rounded + alignment - 1) & ~(alignment - 1);
  uintptr_t image = fs - rounded;
  if (fs > reservation_end || sizeof(struct libc_thread_record) > reservation_end - fs) {
    libc_memory_release_reservation(owned_memory, reservation.identity);
    handle_close(owned_memory);
    return CALL_BAD_REQUEST;
  }
  /* Volatile byte loops keep bootstrap independent of compiler-created libc
   * calls, including optimizations of zero/copy loops. Native backing is zeroed. */
  volatile unsigned char *destination = (void *)image;
  for (size_t index = 0; index < initialized; index++) {
    destination[index] = __tls_template_start[index];
  }
  for (size_t index = initialized; index < rounded + sizeof(struct libc_thread_record); index++) {
    destination[index] = 0;
  }
  struct libc_thread_record *record = (void *)fs;
  record->self = record;
  record->memory = owned_memory;
  record->destructors = &record->first_destructors;
  *context = (struct libc_thread_context){reservation, fs};
  return CALL_OK;
}

enum call_status libc_thread_context_release(handle_t memory,
    struct libc_thread_context *context)
{
  if (!context || !context->reservation.identity || !context->fs_base) {
    return CALL_BAD_REQUEST;
  }
  /* Capture before release. Overflow destructor blocks use separate native
   * regions; a forced stop may leave them populated, but never runs callbacks. */
  struct libc_thread_record *record = (void *)context->fs_base;
  handle_t owned_memory = record->memory;
  struct thread_destructor_block *block = record->destructors;
  struct thread_destructor_block *first = &record->first_destructors;
  enum call_status status = libc_memory_release_reservation(memory, context->reservation.identity);
  if (status != CALL_OK) {
    return status;
  }
  *context = (struct libc_thread_context){0};
  enum call_status cleanup = CALL_OK;
  while (block != first) {
    struct thread_destructor_block *previous = block->previous;
    enum call_status released = libc_memory_release(owned_memory, block->region);
    if (cleanup == CALL_OK && released != CALL_OK) {
      cleanup = released;
    }
    block = previous;
  }
  if (handle_close(owned_memory) != 0 && cleanup == CALL_OK) {
    cleanup = CALL_OUTCOME_UNKNOWN;
  }
  return cleanup;
}

enum call_status libc_thread_collect(handle_t memory, handle_t completion,
    struct libc_thread_context *context, struct thread_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = thread_wait(completion, result);
  if (status != CALL_OK) {
    return status;
  }
  return libc_thread_context_release(memory, context);
}

void libc_thread_bootstrap(const struct startup_info *info)
{
  struct libc_thread_context context;
  struct environment_entry_reply entry;
  enum call_status status = environment_lookup(info->environment_handle, "memory",
      &entry, NULL, 0);
  if (status != CALL_OK || entry.kind != ENVIRONMENT_CAP) {
    libc_start_failed(PROCESS_START_MEMORY, status != CALL_OK ? status : CALL_WRONG_TYPE);
  }
  status = libc_thread_context_prepare(entry.handle, &context);
  if (status != CALL_OK) {
    libc_start_failed(PROCESS_START_TLS, status);
  }
  libc_startup_close(entry.handle, PROCESS_START_MEMORY);
  status = thread_set_fs(info->thread_fs,
      (struct thread_fs_context){context.reservation.identity, context.fs_base});
  if (status != CALL_OK) {
    libc_start_failed(PROCESS_START_FS, status);
  }
  libc_startup_close(info->thread_fs, PROCESS_START_FS);
}
