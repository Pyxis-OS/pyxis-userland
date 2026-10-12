#include <stdlib.h>
#include <syscall.h>
#include "runtime.h"
#include "thread_private.h"
#include "thread_platform.h"
#include "thread_sync.h"

/* C guarantees 32 registrations; further blocks come from the heap. */
#define EXIT_BLOCK_HANDLERS 32

/* Exactly one of the two functions is set. C handlers take no argument;
 * __cxa_atexit handlers, such as C++ static destructors, take one. */
struct exit_handler {
  void (*c_function)(void);
  void (*cxa_function)(void *);
  void *argument;
};

struct exit_block {
  struct exit_block *previous;
  size_t count;
  struct exit_handler handlers[EXIT_BLOCK_HANDLERS];
};

static struct exit_block first_block;
static struct exit_block *current_block = &first_block;
static struct libc_lock exit_guard;
static unsigned int exit_started;

/* The Itanium C++ ABI identifies a module's handlers by the address of its
 * __dso_handle. A static executable is one module. */
void *__dso_handle = &__dso_handle;

/* The SDK linker script defines the bounds. Weak references keep links without
 * it valid; an absent array has equal (null) bounds. */
extern void (*const __fini_array_start[])(void) __attribute__((weak));
extern void (*const __fini_array_end[])(void) __attribute__((weak));

static int register_handler(struct exit_handler handler)
{
  struct exit_block *prepared = NULL;
  for (;;) {
    libc_lock_acquire(&exit_guard);
    if (current_block->count < EXIT_BLOCK_HANDLERS) {
      current_block->handlers[current_block->count++] = handler;
      libc_lock_release(&exit_guard);
      free(prepared);
      return 0;
    }
    if (prepared) {
      prepared->previous = current_block;
      prepared->handlers[prepared->count++] = handler;
      current_block = prepared;
      libc_lock_release(&exit_guard);
      return 0;
    }
    libc_lock_release(&exit_guard);
    prepared = calloc(1, sizeof(*prepared));
    if (!prepared) {
      return -1;
    }
  }
}

int atexit(void (*handler)(void))
{
  return register_handler((struct exit_handler){.c_function = handler});
}

int __cxa_atexit(void (*handler)(void *), void *argument, void *module)
{
  (void)module;
  return register_handler((struct exit_handler){
    .cxa_function = handler,
    .argument = argument,
  });
}

/* A handler may register another; it runs next, before older handlers.
 * Emptied heap blocks stay allocated until the process ends. */
static void run_exit_handlers(void)
{
  for (;;) {
    libc_lock_acquire(&exit_guard);
    if (current_block->count == 0) {
      if (!current_block->previous) {
        libc_lock_release(&exit_guard);
        return;
      }
      current_block = current_block->previous;
      libc_lock_release(&exit_guard);
      continue;
    }
    struct exit_handler handler = current_block->handlers[--current_block->count];
    libc_lock_release(&exit_guard);
    if (handler.c_function) {
      handler.c_function();
    } else {
      handler.cxa_function(handler.argument);
    }
  }
}

static void run_fini_array(void)
{
  for (size_t index = __fini_array_end - __fini_array_start; index > 0; index--) {
    __fini_array_start[index - 1]();
  }
}

[[noreturn]] void _Exit(int status)
{
  syscall1(SYSCALL_EXIT, (uint32_t)status);

  /* A returning exit syscall violates the ABI; do not fall through into code
   * compiled on the assumption that this function never returns. */
  __builtin_trap();
}

[[noreturn]] void exit(int status)
{
  if (!libc_thread_shutdown_ready() ||
      __atomic_exchange_n(&exit_started, 1, __ATOMIC_ACQ_REL)) {
    _Exit(status);
  }
  libc_once_thread_cleanup();
  libc_thread_record()->destroying = true;
  libc_thread_destructors();
  libc_once_thread_cleanup();
  run_exit_handlers();
  run_fini_array();
  stdio_finish();
  _Exit(status);
}

[[noreturn]] void abort(void)
{
  _Exit(EXIT_FAILURE);
}
