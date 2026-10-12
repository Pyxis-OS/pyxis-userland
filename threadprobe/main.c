#include <abi/console.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <abi/wait.h>
#include <clock.h>
#include <console.h>
#include <directory.h>
#include <endpoint.h>
#include <handle.h>
#include <launcher.h>
#include <memory.h>
#include <process.h>
#include <startup.h>
#include <syscall.h>
#include <thread.h>
#include <errno.h>
#include <threads.h>
#include <pyxis/thread_runtime.h>
#include <stdio.h>
#include <string.h>

int threadprobe_runtime_io(void);
int threadprobe_runtime_once(void);
int threadprobe_runtime_cxx(void);
int threadprobe_runtime(bool inspect);
int threadprobe_runtime_time(void);
int threadprobe_runtime_limit(void);

#define PROBE_STACK_BYTES (64 * 1024)
#define PROBE_MARKER UINT64_C(0x7468726561640000)
#define PROBE_TLS_INITIAL UINT32_C(0x746c7307)
#define PROBE_TLS_ALIGNMENT 8192
#define PROBE_KEY_PASSES 4

enum child_action { CHILD_HOLD, CHILD_CALL, CHILD_FAULT, CHILD_PARK };

struct probe_context {
  struct memory_reservation stack;
  struct memory_reservation fs;
  handle_t completion;
  handle_t threads;
  handle_t caller;
  uint64_t marker;
  uint64_t ready;
  uint64_t release;
  uint64_t observed_fs;
  uint64_t observed_rsp;
  uint32_t observed_mxcsr;
  enum child_action action;
  uintptr_t park_address;
  uint64_t park_size, park_rounds;
  uint64_t park_ready, park_done, park_wakes;
  enum call_status park_status;
};

/* Kept in ordinary shared process storage for debugger inspection. */
static struct probe_context contexts[2];
struct tls_probe_context {
  struct probe_context native;
  struct libc_thread_context runtime;
  uintptr_t initialized_address, zero_address, errno_address;
  uint64_t language_order;
  unsigned language_count, key_count;
  int expected_errno;
  bool valid, fault;
};

static struct tls_probe_context tls_contexts[2];
static libc_thread_key callback_key;
static unsigned deleted_key_callbacks;
static _Thread_local uint32_t tls_initialized __attribute__((aligned(PROBE_TLS_ALIGNMENT))) =
    PROBE_TLS_INITIAL;
static _Thread_local unsigned char tls_zero[17];
static uint64_t shared_count;
static uint32_t parking32;
static uint64_t parking64;
static const uint64_t parking_readonly;
static handle_t memory, threads, clock_service;
extern void threadprobe_entry(void);
extern void threadprobe_tls_entry(void);
extern void threadprobe_tls_objects(void *context);
void threadprobe_tls_note(void *argument, unsigned id);
[[noreturn]] void threadprobe_tls_child(struct tls_probe_context *context);
[[noreturn]] void threadprobe_child(struct probe_context *context);

static uint64_t load(uint64_t *word)
{
  return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

static void publish(uint64_t *word, uint64_t value)
{
  __atomic_store_n(word, value, __ATOMIC_RELEASE);
}

[[noreturn]] static void stop_process(int code)
{
  syscall1(SYSCALL_EXIT, (uint64_t)code);
  for (;;) {
    __asm__ volatile("ud2");
  }
}

static uint64_t parking_value(const struct probe_context *context)
{
  return context->park_size == THREAD_ADDRESS_WORD32_BYTES ?
      __atomic_load_n((uint32_t *)context->park_address, __ATOMIC_ACQUIRE) :
      __atomic_load_n((uint64_t *)context->park_address, __ATOMIC_ACQUIRE);
}

static uint64_t parking_child(struct probe_context *context)
{
  for (uint64_t round = 1; round <= context->park_rounds; ++round) {
    publish(&context->park_ready, round);
    while (parking_value(context) < round) {
      enum call_status status = thread_address_wait(context->threads, context->park_address,
          round - 1, context->park_size, THREAD_ADDRESS_NO_DEADLINE);
      if (status == CALL_OK) {
        __atomic_fetch_add(&context->park_wakes, 1, __ATOMIC_ACQ_REL);
      } else if (status != CALL_WOULD_BLOCK) {
        context->park_status = status;
        return status;
      }
    }
    publish(&context->park_done, round);
  }
  return context->marker;
}

[[noreturn]] void threadprobe_child(struct probe_context *context)
{
  __asm__ volatile("mov %%fs:0, %0" : "=r"(context->observed_fs));
  __asm__ volatile("mov %%rsp, %0" : "=r"(context->observed_rsp));
  __asm__ volatile("stmxcsr %0" : "=m"(context->observed_mxcsr));
  __atomic_fetch_add(&shared_count, 1, __ATOMIC_ACQ_REL);
  publish(&context->ready, 1);
  uint64_t value = context->observed_fs;
  if (context->action == CHILD_PARK) {
    value = parking_child(context);
  } else if (context->action == CHILD_CALL) {
    struct endpoint_packet reply;
    value = endpoint_request(context->caller, NULL, 0, NULL, 0, 0, &reply);
  } else {
    while (!load(&context->release)) {
      __asm__ volatile("pause");
    }
    if (context->action == CHILD_FAULT) {
      __asm__ volatile("ud2");
    }
  }
  if (thread_exit(context->threads, value) != CALL_OK) {
    stop_process(1);
  }
  stop_process(1);
}

static enum call_status prepare(struct probe_context *context, unsigned index)
{
  *context = (struct probe_context){.threads = threads, .marker = PROBE_MARKER + index};
  enum call_status status = memory_allocate_guarded(memory, PROBE_STACK_BYTES, &context->stack);
  if (status != CALL_OK) {
    return status;
  }
  status = memory_allocate_guarded(memory, MEMORY_PAGE_SIZE, &context->fs);
  if (status != CALL_OK) {
    memory_release_reservation(memory, context->stack.identity);
    context->stack = (struct memory_reservation){0};
    return status;
  }
  *(uint64_t *)(uintptr_t)(context->fs.address + MEMORY_PAGE_SIZE) = context->marker;
  return CALL_OK;
}

static struct thread_create request_for(struct probe_context *context)
{
  return (struct thread_create){
    .entry = (uintptr_t)threadprobe_entry, .argument = (uintptr_t)context,
    .stack_identity = context->stack.identity, .context_identity = context->fs.identity,
    .fs_base = context->fs.address + MEMORY_PAGE_SIZE,
  };
}

static enum call_status start(struct probe_context *context)
{
  struct thread_create request = request_for(context);
  return thread_create(threads, &request, &context->completion);
}

static bool await_ready(struct probe_context *context)
{
  uint64_t now;
  if (clock_now(clock_service, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(5000000000)) {
    return false;
  }
  uint64_t deadline = now + UINT64_C(5000000000);
  while (!load(&context->ready)) {
    if (clock_now(clock_service, &now) != CALL_OK || now >= deadline ||
        clock_sleep_for(clock_service, UINT64_C(1000000)) != CALL_OK) {
      return false;
    }
  }
  return true;
}

static bool collect(struct probe_context *context, uint64_t expected)
{
  publish(&context->release, 1);
  struct thread_result result, repeat;
  enum call_status status = thread_wait(context->completion, &result);
  if (status != CALL_OK) {
    /* Runtime finalizers must never run while a sibling may remain live. */
    stop_process(1);
  }
  bool valid = result.kind == THREAD_EXITED && result.value == expected &&
      thread_wait(context->completion, &repeat) == CALL_OK &&
      result.kind == repeat.kind && result.value == repeat.value;
  struct wait_interest interest = {.handle = context->completion, .events = WAIT_COMPLETE};
  uint64_t events = 0;
  struct syscall_result readiness = syscall_wait_many(&interest, 1, 0, &events);
  valid &= readiness.status == CALL_OK && readiness.reply_size == sizeof(events) &&
      events == WAIT_COMPLETE;
  valid &= handle_close(context->completion) == 0;
  context->completion = HANDLE_INVALID;
  return valid;
}

static bool release_context(struct probe_context *context)
{
  bool valid = true;
  if (context->fs.identity) {
    valid &= memory_release_reservation(memory, context->fs.identity) == CALL_OK;
  }
  if (context->stack.identity) {
    valid &= memory_release_reservation(memory, context->stack.identity) == CALL_OK;
  }
  context->fs = (struct memory_reservation){0};
  context->stack = (struct memory_reservation){0};
  return valid;
}

static bool prepare_pair(void)
{
  enum call_status first = prepare(&contexts[0], 0);
  enum call_status second = first == CALL_OK ? prepare(&contexts[1], 1) : first;
  if (second != CALL_OK) {
    release_context(&contexts[0]);
    fprintf(stderr, "threadprobe: prepare status %u\n", second);
    return false;
  }
  return true;
}

static int lifecycle(bool inspect)
{
  if (!prepare_pair()) {
    return 1;
  }
  uint32_t inherited_mxcsr;
  __asm__ volatile("stmxcsr %0" : "=m"(inherited_mxcsr));
  printf("threadprobe: contexts=%p shared_count=%p\n", (void *)contexts, (void *)&shared_count);
  for (unsigned i = 0; i < 2; ++i) {
    printf("threadprobe: sibling %u stack=%#llx..%#llx FS=%#llx\n", i,
        (unsigned long long)(contexts[i].stack.address + MEMORY_PAGE_SIZE),
        (unsigned long long)(contexts[i].stack.address + contexts[i].stack.size),
        (unsigned long long)(contexts[i].fs.address + MEMORY_PAGE_SIZE));
  }
  if (inspect) {
    puts("threadprobe: inspect live siblings with GDB, then press Enter to release");
  }
  fflush(stdout);
  for (unsigned i = 0; i < 2; ++i) {
    if (start(&contexts[i]) != CALL_OK || !await_ready(&contexts[i])) {
      stop_process(1);
    }
  }
  bool valid = load(&shared_count) == 2;
  handle_t copy;
  valid &= handle_copy(contexts[0].completion, &copy) == CALL_OK;
  if (copy != HANDLE_INVALID) {
    valid &= handle_close(copy) == 0;
  }
  if (inspect) {
    char byte;
    size_t size;
    handle_t input = startup_stream(STARTUP_STDIN).handle;
    if (console_read(input, &byte, 1, &size) != CALL_OK || size != 1) {
      stop_process(1);
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    valid &= collect(&contexts[i], contexts[i].marker);
    valid &= contexts[i].observed_fs == contexts[i].marker &&
        contexts[i].observed_rsp >= contexts[i].stack.address + MEMORY_PAGE_SIZE &&
        contexts[i].observed_rsp < contexts[i].stack.address + contexts[i].stack.size &&
        contexts[i].observed_mxcsr == inherited_mxcsr;
    valid &= release_context(&contexts[i]);
  }
  printf("threadprobe: lifecycle %s; shared count %llu; distinct stack/FS, repeated WAIT and readiness\n",
      valid ? "passed" : "failed", (unsigned long long)shared_count);
  return valid ? 0 : 1;
}

void threadprobe_tls_note(void *argument, unsigned id)
{
  struct tls_probe_context *context = argument;
  context->valid &= errno == context->expected_errno &&
      tls_initialized == (uint32_t)context->native.marker;
  context->language_order = context->language_order * 10 + id;
  ++context->language_count;
}

static void tls_key_callback(void *argument)
{
  struct tls_probe_context *context = argument;
  void *value = context;
  context->valid &= context->language_count == 2 && context->language_order == 21;
  context->valid &= libc_thread_key_get(callback_key, &value) == CALL_OK && value == NULL;
  context->valid &= errno == context->expected_errno &&
      tls_initialized == (uint32_t)context->native.marker;
  ++context->key_count;
  /* Reassign even on the last permitted pass: exit must stop after four. */
  context->valid &= libc_thread_key_set(callback_key, context) == CALL_OK;
}

static void tls_deleted_callback(void *argument)
{
  (void)argument;
  __atomic_fetch_add(&deleted_key_callbacks, 1, __ATOMIC_RELAXED);
}

[[noreturn]] void threadprobe_tls_child(struct tls_probe_context *context)
{
  struct probe_context *native = &context->native;
  __asm__ volatile("mov %%fs:0, %0" : "=r"(native->observed_fs));
  __asm__ volatile("mov %%rsp, %0" : "=r"(native->observed_rsp));
  context->initialized_address = (uintptr_t)&tls_initialized;
  context->zero_address = (uintptr_t)tls_zero;
  context->errno_address = (uintptr_t)&errno;
  context->valid = tls_initialized == PROBE_TLS_INITIAL && errno == 0 &&
      context->initialized_address % PROBE_TLS_ALIGNMENT == 0;
  for (size_t i = 0; i < sizeof(tls_zero); ++i) {
    context->valid &= tls_zero[i] == 0;
    tls_zero[i] = (unsigned char)(native->marker + i);
  }
  tls_initialized = (uint32_t)native->marker;
  errno = context->expected_errno;
  void *value = context;
  context->valid &= libc_thread_key_get(callback_key, &value) == CALL_OK && value == NULL;
  context->valid &= libc_thread_key_set(callback_key, context) == CALL_OK;
  threadprobe_tls_objects(context);
  publish(&native->ready, 1);
  while (!load(&native->release)) {
    __asm__ volatile("pause");
  }
  context->valid &= errno == context->expected_errno &&
      tls_initialized == (uint32_t)native->marker;
  for (size_t i = 0; i < sizeof(tls_zero); ++i) {
    context->valid &= tls_zero[i] == (unsigned char)(native->marker + i);
  }
  if (context->fault) {
    __asm__ volatile("ud2");
  }
  libc_thread_exit(native->threads, native->marker);
}

static bool tls_prepare_pair(void)
{
  if (libc_thread_key_create(tls_key_callback, &callback_key) != CALL_OK) {
    return false;
  }
  for (unsigned i = 0; i < 2; ++i) {
    struct tls_probe_context *context = &tls_contexts[i];
    *context = (struct tls_probe_context){
      .native = {.threads = threads, .marker = PROBE_MARKER + i + 1},
      .expected_errno = i ? ERANGE : ENOENT,
    };
    if (memory_allocate_guarded(memory, PROBE_STACK_BYTES, &context->native.stack) != CALL_OK ||
        libc_thread_context_prepare(memory, &context->runtime) != CALL_OK) {
      for (unsigned j = 0; j <= i; ++j) {
        libc_thread_context_release(memory, &tls_contexts[j].runtime);
        release_context(&tls_contexts[j].native);
      }
      libc_thread_key_delete(callback_key);
      return false;
    }
  }
  return true;
}

static enum call_status tls_start(struct tls_probe_context *context)
{
  struct thread_create request = {
    .entry = (uintptr_t)threadprobe_tls_entry, .argument = (uintptr_t)context,
    .stack_identity = context->native.stack.identity,
    .context_identity = context->runtime.reservation.identity,
    .fs_base = context->runtime.fs_base,
  };
  return thread_create(threads, &request, &context->native.completion);
}

static bool tls_key_refusal(void)
{
  libc_thread_key stale = 0, replacement = 0;
  void *value = &stale;
  bool valid = libc_thread_key_create(NULL, NULL) == CALL_BAD_REQUEST &&
      libc_thread_key_get(0, &value) == CALL_BAD_REQUEST && value == NULL &&
      libc_thread_key_get(callback_key, NULL) == CALL_BAD_REQUEST &&
      libc_thread_key_set(0, &stale) == CALL_BAD_REQUEST &&
      libc_thread_key_delete(0) == CALL_BAD_REQUEST;
  if (libc_thread_key_create(tls_deleted_callback, &stale) != CALL_OK) {
    return false;
  }
  valid &= libc_thread_key_set(stale, &stale) == CALL_OK;
  valid &= libc_thread_key_delete(stale) == CALL_OK;
  value = &stale;
  valid &= libc_thread_key_get(stale, &value) == CALL_BAD_REQUEST && value == NULL;
  valid &= libc_thread_key_set(stale, &stale) == CALL_BAD_REQUEST &&
      libc_thread_key_delete(stale) == CALL_BAD_REQUEST;
  if (libc_thread_key_create(tls_deleted_callback, &replacement) != CALL_OK) {
    return false;
  }
  value = &stale;
  valid &= replacement != stale &&
      libc_thread_key_get(replacement, &value) == CALL_OK && value == NULL;
  valid &= libc_thread_key_set(replacement, &replacement) == CALL_OK;
  value = &stale;
  valid &= libc_thread_key_get(stale, &value) == CALL_BAD_REQUEST && value == NULL &&
      libc_thread_key_set(stale, &stale) == CALL_BAD_REQUEST &&
      libc_thread_key_delete(stale) == CALL_BAD_REQUEST;
  valid &= libc_thread_key_get(replacement, &value) == CALL_OK && value == &replacement;
  valid &= libc_thread_key_delete(replacement) == CALL_OK;
  return valid && __atomic_load_n(&deleted_key_callbacks, __ATOMIC_RELAXED) == 0;
}

static int tls_lifecycle(bool inspect)
{
  bool valid = errno == 0 && tls_initialized == PROBE_TLS_INITIAL;
  for (size_t i = 0; i < sizeof(tls_zero); ++i) {
    valid &= tls_zero[i] == 0;
  }
  tls_initialized = UINT32_C(0x70617265);
  tls_zero[0] = 0xa5;
  errno = EINVAL;
  if (!tls_prepare_pair()) {
    fputs("threadprobe: TLS prepare failed\n", stderr);
    return 1;
  }
  valid &= tls_key_refusal();
  printf("threadprobe: TLS contexts=%p parent TLS=%p errno=%p\n", (void *)tls_contexts,
      (void *)&tls_initialized, (void *)&errno);
  for (unsigned i = 0; i < 2; ++i) {
    printf("threadprobe: TLS sibling %u stack=%#llx..%#llx FS=%#llx backing=%#llx..%#llx\n", i,
        (unsigned long long)(tls_contexts[i].native.stack.address + MEMORY_PAGE_SIZE),
        (unsigned long long)(tls_contexts[i].native.stack.address + tls_contexts[i].native.stack.size),
        (unsigned long long)tls_contexts[i].runtime.fs_base,
        (unsigned long long)tls_contexts[i].runtime.reservation.address,
        (unsigned long long)(tls_contexts[i].runtime.reservation.address +
            tls_contexts[i].runtime.reservation.size));
  }
  if (inspect) {
    puts("threadprobe: inspect shared CR3, guarded stack/TLS and errno with GDB, then Enter");
  }
  fflush(stdout);
  for (unsigned i = 0; i < 2; ++i) {
    if (tls_start(&tls_contexts[i]) != CALL_OK || !await_ready(&tls_contexts[i].native)) {
      stop_process(1);
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    valid &= memory_release_reservation(memory, tls_contexts[i].runtime.reservation.identity) ==
        CALL_BUSY;
    valid &= memory_release_reservation(memory, tls_contexts[i].native.stack.identity) == CALL_BUSY;
  }
  if (inspect) {
    char byte;
    size_t size;
    if (console_read(startup_stream(STARTUP_STDIN).handle, &byte, 1, &size) != CALL_OK || size != 1) {
      stop_process(1);
    }
  }
  for (unsigned i = 0; i < 2; ++i) {
    struct tls_probe_context *context = &tls_contexts[i];
    struct probe_context *native = &context->native;
    uintptr_t expected_fs = context->runtime.fs_base;
    publish(&native->release, 1);
    struct thread_result result, repeat;
    enum call_status status = libc_thread_collect(memory, native->completion, &context->runtime, &result);
    if (status != CALL_OK) {
      stop_process(1);
    }
    valid &= result.kind == THREAD_EXITED && result.value == native->marker &&
        thread_wait(native->completion, &repeat) == CALL_OK &&
        repeat.kind == result.kind && repeat.value == result.value;
    valid &= !context->runtime.reservation.identity && !context->runtime.fs_base &&
        native->observed_fs == expected_fs &&
        native->observed_rsp >= native->stack.address + MEMORY_PAGE_SIZE &&
        native->observed_rsp < native->stack.address + native->stack.size;
    valid &= context->valid && context->language_count == 2 && context->language_order == 21 &&
        context->key_count == PROBE_KEY_PASSES;
    valid &= handle_close(native->completion) == 0;
    native->completion = HANDLE_INVALID;
    valid &= release_context(native);
  }
  valid &= tls_contexts[0].initialized_address != tls_contexts[1].initialized_address &&
      tls_contexts[0].initialized_address != (uintptr_t)&tls_initialized &&
      tls_contexts[1].initialized_address != (uintptr_t)&tls_initialized &&
      tls_contexts[0].zero_address != tls_contexts[1].zero_address &&
      tls_contexts[0].errno_address != tls_contexts[1].errno_address &&
      tls_contexts[0].errno_address != (uintptr_t)&errno &&
      tls_contexts[1].errno_address != (uintptr_t)&errno;
  valid &= errno == EINVAL && tls_initialized == UINT32_C(0x70617265) && tls_zero[0] == 0xa5;
  valid &= libc_thread_key_delete(callback_key) == CALL_OK &&
      __atomic_load_n(&deleted_key_callbacks, __ATOMIC_RELAXED) == 0;
  printf("threadprobe: TLS %s; alignment=%u distinct TLS/errno; C++ LIFO=21; key callbacks=%u/%u; BUSY, collect/repeated WAIT\n",
      valid ? "passed" : "failed", PROBE_TLS_ALIGNMENT,
      tls_contexts[0].key_count, tls_contexts[1].key_count);
  return valid ? 0 : 1;
}

static uint64_t parking_deadline(void)
{
  uint64_t now;
  if (clock_now(clock_service, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(5000000000)) {
    stop_process(1);
  }
  return now + UINT64_C(5000000000);
}

static void parking_poll(uint64_t deadline)
{
  uint64_t now;
  if (clock_now(clock_service, &now) != CALL_OK || now >= deadline) {
    stop_process(1);
  }
}

static void parking_pause(uint64_t deadline)
{
  uint64_t now;
  if (clock_now(clock_service, &now) != CALL_OK || now >= deadline ||
      clock_sleep_for(clock_service, UINT64_C(1000000)) != CALL_OK) {
    stop_process(1);
  }
}

static void parking_enter(void)
{
  char byte;
  size_t size;
  if (console_read(startup_stream(STARTUP_STDIN).handle, &byte, 1, &size) != CALL_OK || size != 1) {
    stop_process(1);
  }
}

static uint64_t parking_wake(uintptr_t address, uint64_t word_size, uint64_t maximum)
{
  uint64_t woken;
  if (thread_address_wake(threads, address, word_size, maximum, &woken) != CALL_OK) {
    stop_process(1);
  }
  return woken;
}

static int parking_contention(uint64_t word_size, bool inspect)
{
  if (!prepare_pair()) {
    return 1;
  }
  uintptr_t address = word_size == THREAD_ADDRESS_WORD32_BYTES ?
      (uintptr_t)&parking32 : (uintptr_t)&parking64;
  uint64_t rounds = inspect ? 1 : 64;
  for (unsigned i = 0; i < 2; ++i) {
    contexts[i].action = CHILD_PARK;
    contexts[i].park_address = address;
    contexts[i].park_size = word_size;
    contexts[i].park_rounds = rounds;
  }
  printf("threadprobe: parking word=%p size=%llu contexts=%p rounds=%llu\n",
      (void *)address, (unsigned long long)word_size, (void *)contexts,
      (unsigned long long)rounds);
  if (inspect) {
    puts("threadprobe: inspect both parked registrations with GDB, then press Enter");
  }
  fflush(stdout);
  for (unsigned i = 0; i < 2; ++i) {
    if (start(&contexts[i]) != CALL_OK || !await_ready(&contexts[i])) {
      stop_process(1);
    }
  }
  uint64_t woken = 0;
  for (uint64_t round = 1; round <= rounds; ++round) {
    uint64_t deadline = parking_deadline();
    while (load(&contexts[0].park_ready) < round || load(&contexts[1].park_ready) < round) {
      parking_pause(deadline);
    }
    if (inspect) {
      parking_enter();
      deadline = parking_deadline();
    } else {
      uint64_t before = load(&contexts[0].park_wakes) + load(&contexts[1].park_wakes);
      uint64_t count;
      while (!(count = parking_wake(address, word_size, 1))) {
        parking_poll(deadline);
      }
      woken += count;
      /* Wake with the predicate unchanged, proving a real parked registration
       * and requiring the child to recheck rather than treat wake as ownership. */
      while (load(&contexts[0].park_wakes) + load(&contexts[1].park_wakes) == before) {
        parking_pause(deadline);
      }
    }
    if (word_size == THREAD_ADDRESS_WORD32_BYTES) {
      __atomic_store_n(&parking32, (uint32_t)round, __ATOMIC_RELEASE);
    } else {
      publish(&parking64, round);
    }
    while (load(&contexts[0].park_done) < round || load(&contexts[1].park_done) < round) {
      woken += parking_wake(address, word_size, UINT64_MAX);
      if (inspect) {
        parking_pause(deadline);
      } else {
        parking_poll(deadline);
      }
    }
  }
  bool valid = true;
  for (unsigned i = 0; i < 2; ++i) {
    valid &= collect(&contexts[i], contexts[i].marker);
    valid &= contexts[i].park_status == CALL_OK;
    valid &= release_context(&contexts[i]);
  }
  uint64_t observed = load(&contexts[0].park_wakes) + load(&contexts[1].park_wakes);
  valid &= observed == woken && woken >= (inspect ? 2 : rounds);
  valid &= parking_wake(address, word_size, UINT64_MAX) == 0;
  printf("threadprobe: parking %s; size=%llu rounds=%llu woken=%llu observed=%llu; empty after completion\n",
      valid ? "passed" : "failed", (unsigned long long)word_size,
      (unsigned long long)rounds, (unsigned long long)woken, (unsigned long long)observed);
  return valid ? 0 : 1;
}

static int parking_refusal(void)
{
  uintptr_t address = (uintptr_t)&parking64;
  uint64_t stack_word = 0;
  uint64_t now, woken = UINT64_MAX;
  bool valid = thread_address_wait(threads, address, 1, THREAD_ADDRESS_WORD64_BYTES, 0) ==
      CALL_WOULD_BLOCK;
  valid &= thread_address_wait(threads, address, 0, THREAD_ADDRESS_WORD64_BYTES, 0) == CALL_TIMED_OUT;
  if (clock_now(clock_service, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(20000000)) {
    return 1;
  }
  valid &= thread_address_wait(threads, address, 0, THREAD_ADDRESS_WORD64_BYTES,
      now + UINT64_C(20000000)) == CALL_TIMED_OUT;
  valid &= thread_address_wake(threads, address, THREAD_ADDRESS_WORD64_BYTES, UINT64_MAX,
      &woken) == CALL_OK && woken == 0;
  valid &= thread_address_wait(threads, (uintptr_t)&stack_word, 1,
      THREAD_ADDRESS_WORD64_BYTES, 0) == CALL_WOULD_BLOCK;
  struct memory_region region;
  if (memory_allocate(memory, MEMORY_PAGE_SIZE, &region) != CALL_OK) {
    return 1;
  }
  if (clock_now(clock_service, &now) != CALL_OK || now > UINT64_MAX - UINT64_C(20000000)) {
    memory_release(memory, region);
    return 1;
  }
  valid &= thread_address_wait(threads, region.address, 0, THREAD_ADDRESS_WORD64_BYTES,
      now + UINT64_C(20000000)) == CALL_TIMED_OUT;
  valid &= thread_address_wake(threads, region.address, THREAD_ADDRESS_WORD64_BYTES, UINT64_MAX,
      &woken) == CALL_OK && woken == 0;
  valid &= memory_release(memory, region) == CALL_OK;
  valid &= thread_address_wait(threads, address + 1, 0, THREAD_ADDRESS_WORD64_BYTES, 0) ==
      CALL_BAD_REQUEST;
  valid &= thread_address_wait(threads, 0, 0, THREAD_ADDRESS_WORD64_BYTES, 0) == CALL_BAD_BUFFER;
  valid &= thread_address_wait(threads, (uintptr_t)&parking_readonly, 0,
      THREAD_ADDRESS_WORD64_BYTES, 0) == CALL_BAD_BUFFER;
  valid &= thread_address_wait(threads, (uintptr_t)&parking32, UINT64_C(1) << 32,
      THREAD_ADDRESS_WORD32_BYTES, 0) == CALL_BAD_REQUEST;
  woken = UINT64_MAX;
  valid &= thread_address_wake(threads, address, THREAD_ADDRESS_WORD64_BYTES, 0, &woken) ==
      CALL_BAD_REQUEST && woken == 0;
  woken = UINT64_MAX;
  valid &= thread_address_wake(threads, (uintptr_t)&parking_readonly,
      THREAD_ADDRESS_WORD64_BYTES, 1, &woken) == CALL_BAD_BUFFER && woken == 0;
  printf("threadprobe: parking refusals %s; .bss/stack/heap, mismatch, poll/future expiry, empty wake and bad addresses\n",
      valid ? "passed" : "failed");
  return valid ? 0 : 1;
}

static int parking_unmap(void)
{
  if (!prepare_pair()) {
    return 1;
  }
  struct memory_region region, replacement;
  if (memory_allocate(memory, MEMORY_PAGE_SIZE, &region) != CALL_OK) {
    release_context(&contexts[0]);
    release_context(&contexts[1]);
    return 1;
  }
  contexts[0].action = CHILD_PARK;
  contexts[0].park_address = region.address;
  contexts[0].park_size = THREAD_ADDRESS_WORD64_BYTES;
  contexts[0].park_rounds = 1;
  printf("threadprobe: withdrawal word=%#llx context=%p; inspect its parked registration with GDB, then Enter\n",
      (unsigned long long)region.address, (void *)&contexts[0]);
  fflush(stdout);
  if (start(&contexts[0]) != CALL_OK || !await_ready(&contexts[0])) {
    stop_process(1);
  }
  parking_enter();
  if (memory_release(memory, region) != CALL_OK ||
      memory_allocate(memory, MEMORY_PAGE_SIZE, &replacement) != CALL_OK) {
    stop_process(1);
  }
  bool same_address = replacement.address == region.address;
  bool valid = parking_wake(replacement.address, THREAD_ADDRESS_WORD64_BYTES, UINT64_MAX) == 0;
  valid &= collect(&contexts[0], CALL_BAD_BUFFER);
  valid &= contexts[0].park_status == CALL_BAD_BUFFER;
  valid &= memory_release(memory, replacement) == CALL_OK;
  valid &= release_context(&contexts[0]);
  valid &= release_context(&contexts[1]);
  printf("threadprobe: withdrawal %s; old WAIT BAD_BUFFER, new mapping has no old waiter; reused address=%u\n",
      valid ? "passed" : "failed", same_address);
  return valid ? 0 : 1;
}

static int bind_initial(void)
{
  if (!prepare_pair()) {
    return 1;
  }
  struct thread_fs_context first = {contexts[0].fs.identity,
      contexts[0].fs.address + MEMORY_PAGE_SIZE};
  struct thread_fs_context second = {contexts[1].fs.identity,
      contexts[1].fs.address + MEMORY_PAGE_SIZE};
  enum call_status bind = thread_set_fs(threads, first);
  enum call_status rebind = thread_set_fs(threads, second);
  bool valid = bind == CALL_BUSY && rebind == CALL_BUSY;
  valid &= release_context(&contexts[0]);
  valid &= release_context(&contexts[1]);
  printf("threadprobe: initial libc FS %s; both bind attempts BUSY (%u, %u), unbound contexts released\n",
      valid ? "passed" : "failed", bind, rebind);
  return valid ? 0 : 1;
}

static int refusal(void)
{
  if (!prepare_pair()) {
    return 1;
  }
  puts("threadprobe: guarded context refusals");
  fflush(stdout);
  if (start(&contexts[0]) != CALL_OK || !await_ready(&contexts[0])) {
    stop_process(1);
  }
  enum call_status stack_release = memory_release_reservation(memory, contexts[0].stack.identity);
  enum call_status fs_release = memory_release_reservation(memory, contexts[0].fs.identity);
  struct thread_create request = request_for(&contexts[1]);
  request.stack_identity = contexts[0].stack.identity;
  handle_t unexpected;
  enum call_status duplicate_stack = thread_create(threads, &request, &unexpected);
  if (duplicate_stack == CALL_OK || duplicate_stack == CALL_OUTCOME_UNKNOWN) {
    stop_process(1);
  }
  request = request_for(&contexts[1]);
  request.context_identity = contexts[0].fs.identity;
  request.fs_base = contexts[0].fs.address + MEMORY_PAGE_SIZE;
  enum call_status duplicate_fs = thread_create(threads, &request, &unexpected);
  if (duplicate_fs == CALL_OK || duplicate_fs == CALL_OUTCOME_UNKNOWN) {
    stop_process(1);
  }
  bool valid = stack_release == CALL_BUSY && fs_release == CALL_BUSY &&
      duplicate_stack == CALL_BUSY && duplicate_fs == CALL_BUSY;
  valid &= collect(&contexts[0], contexts[0].marker);
  uint64_t stale_identity = contexts[0].fs.identity;
  valid &= release_context(&contexts[0]);
  request = request_for(&contexts[1]);
  request.context_identity = stale_identity;
  enum call_status stale = thread_create(threads, &request, &unexpected);
  if (stale == CALL_OK || stale == CALL_OUTCOME_UNKNOWN) {
    stop_process(1);
  }
  request = request_for(&contexts[1]);
  request.entry = contexts[1].fs.address + MEMORY_PAGE_SIZE;
  enum call_status nx = thread_create(threads, &request, &unexpected);
  if (nx == CALL_OK || nx == CALL_OUTCOME_UNKNOWN) {
    stop_process(1);
  }
  valid &= stale != CALL_OK && nx != CALL_OK;
  /* Successful reuse after the two failed creates checks provisional pin unwind. */
  if (start(&contexts[1]) != CALL_OK || !await_ready(&contexts[1])) {
    stop_process(1);
  }
  valid &= collect(&contexts[1], contexts[1].marker);
  valid &= release_context(&contexts[1]);
  printf("threadprobe: refusals %s; pinned=%u/%u duplicate=%u/%u stale=%u NX=%u\n",
      valid ? "passed" : "failed", stack_release, fs_release, duplicate_stack, duplicate_fs, stale, nx);
  return valid ? 0 : 1;
}

struct held_region {
  struct held_region *next;
  struct memory_region region;
};

static int allocation_child(void *argument)
{
  (void)argument;
  return 23;
}

static int exhaustion(void)
{
  enum call_status status = prepare(&contexts[0], 0);
  if (status != CALL_OK) {
    fprintf(stderr, "threadprobe: prepare status %u\n", status);
    return 1;
  }
  puts("threadprobe: exhausting real private backing before CREATE; process-local allocations only");
  fflush(stdout);
  struct held_region *head = NULL;
  size_t count = 0, bytes = 0;
  enum call_status allocation_status = CALL_OK;
  for (unsigned phase = 0; phase < 2; ++phase) {
    size_t chunk = phase ? MEMORY_PAGE_SIZE : 1024 * 1024;
    struct memory_region region;
    while ((allocation_status = memory_allocate(memory, chunk, &region)) == CALL_OK) {
      struct held_region *node = (void *)(uintptr_t)region.address;
      *node = (struct held_region){head, region};
      head = node;
      ++count;
      bytes += region.size;
    }
  }
  thrd_t managed;
  int managed_status = thrd_create(&managed, allocation_child, NULL);
  if (managed_status == thrd_success) {
    if (thrd_join(managed, NULL) != thrd_success) {
      stop_process(1);
    }
  }
  enum call_status create_status = start(&contexts[0]);
  bool valid = allocation_status == CALL_NO_MEMORY && create_status == CALL_NO_MEMORY &&
      managed_status == thrd_nomem;
  if (create_status == CALL_OK) {
    valid &= collect(&contexts[0], contexts[0].marker);
  } else if (create_status == CALL_OUTCOME_UNKNOWN) {
    stop_process(1);
  }
  while (head) {
    struct held_region *next = head->next;
    struct memory_region region = head->region;
    valid &= memory_release(memory, region) == CALL_OK;
    head = next;
  }
  if (start(&contexts[0]) != CALL_OK || !await_ready(&contexts[0])) {
    stop_process(1);
  }
  valid &= collect(&contexts[0], contexts[0].marker);
  valid &= release_context(&contexts[0]);
  int managed_result;
  if (thrd_create(&managed, allocation_child, NULL) != thrd_success ||
      thrd_join(managed, &managed_result) != thrd_success) {
    stop_process(1);
  }
  valid &= managed_result == 23;
  printf("threadprobe: exhaustion %s; %zu regions %zu bytes, allocation=%u CREATE=%u libc=%d, native/libc reuse after cleanup\n",
      valid ? "passed" : "failed", count, bytes, allocation_status, create_status, managed_status);
  return valid ? 0 : 1;
}

static int close_race(void)
{
  struct endpoint_create_reply endpoint;
  if (!prepare_pair() || endpoint_create(startup_resource("service"), &endpoint) != CALL_OK) {
    return 1;
  }
  contexts[0].action = CHILD_CALL;
  contexts[0].caller = endpoint.caller;
  puts("threadprobe: close caller/receiver while sibling CALL is admitted; release pinned stack");
  fflush(stdout);
  if (start(&contexts[0]) != CALL_OK || !await_ready(&contexts[0])) {
    stop_process(1);
  }
  struct endpoint_packet packet;
  if (endpoint_receive(endpoint.receiver, &packet) != CALL_OK) {
    stop_process(1);
  }
  enum call_status pinned = memory_release_reservation(memory, contexts[0].stack.identity);
  bool valid = packet.kind == ENDPOINT_MESSAGE_CALL && pinned == CALL_BUSY;
  valid &= handle_close(endpoint.caller) == 0;
  valid &= handle_close(endpoint.receiver) == 0;
  valid &= collect(&contexts[0], CALL_ENDPOINT_CLOSED);
  valid &= handle_close(packet.receipt) == 0;
  valid &= release_context(&contexts[0]);
  valid &= release_context(&contexts[1]);
  printf("threadprobe: concurrent CALL/CLOSE/RELEASE %s; pinned release=%u\n",
      valid ? "passed" : "failed", pinned);
  return valid ? 0 : 1;
}

[[noreturn]] static void supervised_child(bool fault, bool parking, bool tls)
{
  if (tls) {
    if (!tls_prepare_pair()) {
      stop_process(1);
    }
    for (unsigned i = 0; i < 2; ++i) {
      tls_contexts[i].fault = fault && i == 0;
      if (tls_start(&tls_contexts[i]) != CALL_OK || !await_ready(&tls_contexts[i].native) ||
          !tls_contexts[i].valid) {
        stop_process(1);
      }
    }
  } else if (!prepare_pair()) {
    stop_process(1);
  }
  contexts[0].action = fault ? CHILD_FAULT : CHILD_HOLD;
  if (parking) {
    for (unsigned i = 0; i < 2; ++i) {
      contexts[i].action = CHILD_PARK;
      contexts[i].park_address = (uintptr_t)&parking64;
      contexts[i].park_size = THREAD_ADDRESS_WORD64_BYTES;
      contexts[i].park_rounds = 1;
    }
  }
  if (!tls) {
    for (unsigned i = 0; i < 2; ++i) {
      if (start(&contexts[i]) != CALL_OK || !await_ready(&contexts[i])) {
        stop_process(1);
      }
    }
  }
  if (parking) {
    uint64_t deadline = parking_deadline();
    while (!load(&contexts[0].park_wakes) || !load(&contexts[1].park_wakes)) {
      parking_wake((uintptr_t)&parking64, THREAD_ADDRESS_WORD64_BYTES, UINT64_MAX);
      parking_pause(deadline);
    }
  }
  struct endpoint_grant observers[2] = {
    {tls ? tls_contexts[0].native.completion : contexts[0].completion, THREAD_COMPLETION_RIGHT_WAIT, 0},
    {tls ? tls_contexts[1].native.completion : contexts[1].completion, THREAD_COMPLETION_RIGHT_WAIT, 0},
  };
  struct endpoint_packet reply;
  if (endpoint_request(startup_resource("bootstrap"), NULL, 0, observers, 2, 0, &reply) != CALL_OK) {
    stop_process(1);
  }
  if (fault) {
    publish(tls ? &tls_contexts[0].native.release : &contexts[0].release, 1);
  }
  for (;;) {
    __asm__ volatile("pause");
  }
}

static int supervise(bool fault, bool parking, bool tls)
{
  struct endpoint_create_reply endpoint;
  handle_t image = HANDLE_INVALID, child = HANDLE_INVALID;
  enum call_status status = endpoint_create(startup_resource("service"), &endpoint);
  if (status != CALL_OK) {
    fprintf(stderr, "threadprobe: endpoint status %u\n", status);
    return 1;
  }
  status = directory_lookup(startup_root("bin"), "threadprobe.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  enum { MEMORY, THREADS, CLOCK, BOOTSTRAP, GRANT_COUNT };
  struct launch_grant grants[GRANT_COUNT] = {
    [MEMORY] = {memory, MEMORY_RIGHT_MANAGE, 0},
    [THREADS] = {threads, THREAD_RIGHT_MANAGE, 0},
    [CLOCK] = {clock_service, CLOCK_RIGHTS, 0},
    [BOOTSTRAP] = {endpoint.caller, 0, HANDLE_TRANSPORT_SEND | HANDLE_TRANSPORT_RECEIVE},
  };
  struct launch_binding resources[GRANT_COUNT] = {
    {(uintptr_t)"memory", MEMORY}, {(uintptr_t)"threads", THREADS},
    {(uintptr_t)"clock", CLOCK}, {(uintptr_t)"bootstrap", BOOTSTRAP},
  };
  const char *mode = tls ? (fault ? "--child-tls-fault" : "--child-tls-stop") :
      parking ? "--child-park-stop" : fault ? "--child-fault" : "--child-stop";
  uint64_t arguments[] = {(uintptr_t)"threadprobe", (uintptr_t)mode};
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = GRANT_COUNT,
    .resources = (uintptr_t)resources, .resource_count = GRANT_COUNT,
    .argv = (uintptr_t)arguments, .argc = 2,
  };
  if (status == CALL_OK) {
    status = launcher_launch(startup_resource("launcher"), &request, &child);
  }
  bool valid = status == CALL_OK;
  uint64_t supervisor_woken = UINT64_MAX;
  struct endpoint_packet packet;
  if (valid) {
    uint64_t now, events;
    struct wait_interest interest = {.handle = endpoint.receiver, .events = WAIT_READABLE};
    status = clock_now(clock_service, &now);
    if (status == CALL_OK && now <= UINT64_MAX - UINT64_C(10000000000)) {
      struct syscall_result wait = syscall_wait_many(&interest, 1,
          now + UINT64_C(10000000000), &events);
      status = wait.status < CALL_STATUS_COUNT ? wait.status : CALL_OUTCOME_UNKNOWN;
      if (wait.reply_size != (status == CALL_OK ? sizeof(events) : 0)) {
        status = CALL_OUTCOME_UNKNOWN;
      }
      if (status == CALL_OK && !(events & WAIT_READABLE)) {
        status = CALL_UNAVAILABLE;
      }
    } else {
      status = CALL_LIMIT;
    }
    if (status == CALL_OK) {
      status = endpoint_receive(endpoint.receiver, &packet);
    }
    valid = status == CALL_OK && packet.kind == ENDPOINT_MESSAGE_CALL && packet.grant_count == 2;
    if (valid) {
      if (parking) {
        valid = thread_address_wake(threads, (uintptr_t)&parking64,
            THREAD_ADDRESS_WORD64_BYTES, UINT64_MAX, &supervisor_woken) == CALL_OK &&
            supervisor_woken == 0;
      }
      status = fault ? endpoint_reply(packet.receipt, 0, NULL, 0, NULL, 0) : process_terminate(child);
      valid &= status == CALL_OK;
      if (!valid) {
        process_terminate(child);
      }
      for (unsigned i = 0; i < 2; ++i) {
        struct thread_result result;
        valid &= thread_wait(packet.grants[i].handle, &result) == CALL_OK &&
            result.kind == THREAD_STOPPED && result.value == 0;
        valid &= handle_close(packet.grants[i].handle) == 0;
      }
      if (!fault) {
        valid &= handle_close(packet.receipt) == 0;
      }
    }
    if (!valid) {
      process_terminate(child);
    }
    struct process_result result;
    valid &= process_wait(child, &result) == CALL_OK &&
        result.kind == (fault ? PROCESS_FAULTED : PROCESS_TERMINATED);
    valid &= handle_close(child) == 0;
  }
  if (image != HANDLE_INVALID) {
    valid &= handle_close(image) == 0;
  }
  valid &= handle_close(endpoint.caller) == 0;
  valid &= handle_close(endpoint.receiver) == 0;
  printf("threadprobe: process %s %s; live sibling completions STOPPED, process reclaimed (status %u)\n",
      tls ? (fault ? "tls-fault" : "tls-stop") : parking ? "park-stop" : fault ? "fault" : "stop",
      valid ? "passed" : "failed", status);
  if (parking) {
    printf("threadprobe: process-key isolation %s; supervisor same-address WAKE count %llu\n",
        valid ? "passed" : "failed", (unsigned long long)supervisor_woken);
  }
  return valid ? 0 : 1;
}

int main(int argc, char **argv)
{
  memory = startup_resource("memory");
  threads = startup_resource("threads");
  clock_service = startup_resource("clock");
  if (argc != 2) {
    fputs("usage: session bin://threadprobe.pxe "
        "lifecycle|inspect|tls|tls-inspect|tls-stop|tls-fault|bind|refusal|exhaust|stop|fault|close|"
        "park32|park64|park-refusal|park-inspect|park-unmap|park-stop|runtime|runtime-inspect|runtime-time|runtime-limit|runtime-cxx|runtime-io|runtime-once\n", stderr);
    return 1;
  }
  const struct {
    const char *name;
    handle_t handle;
  } required[] = {{"memory", memory}, {"threads", threads}, {"clock", clock_service}};
  bool missing = false;
  for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
    if (required[i].handle == HANDLE_INVALID) {
      fprintf(stderr, "threadprobe: missing native startup grant '%s'\n", required[i].name);
      missing = true;
    }
  }
  if (missing) {
    return 1;
  }
  if (!strcmp(argv[1], "--child-stop") || !strcmp(argv[1], "--child-fault") ||
      !strcmp(argv[1], "--child-park-stop") || !strcmp(argv[1], "--child-tls-stop") ||
      !strcmp(argv[1], "--child-tls-fault")) {
    supervised_child(!strcmp(argv[1], "--child-fault") || !strcmp(argv[1], "--child-tls-fault"),
        !strcmp(argv[1], "--child-park-stop"),
        !strcmp(argv[1], "--child-tls-stop") || !strcmp(argv[1], "--child-tls-fault"));
  }
  if (!strcmp(argv[1], "runtime") || !strcmp(argv[1], "runtime-inspect")) {
    return threadprobe_runtime(!strcmp(argv[1], "runtime-inspect"));
  }
  if (!strcmp(argv[1], "runtime-io")) {
    return threadprobe_runtime_io();
  }
  if (!strcmp(argv[1], "runtime-once")) {
    return threadprobe_runtime_once();
  }
  if (!strcmp(argv[1], "runtime-cxx")) {
    int failures = threadprobe_runtime_cxx();
    printf("threadprobe: runtime-cxx %s; failure bits=%#x\n", failures ? "failed" : "passed", failures);
    return failures ? 1 : 0;
  }
  if (!strcmp(argv[1], "runtime-time")) {
    return threadprobe_runtime_time();
  }
  if (!strcmp(argv[1], "runtime-limit")) {
    return threadprobe_runtime_limit();
  }
  if (!strcmp(argv[1], "lifecycle") || !strcmp(argv[1], "inspect")) {
    return lifecycle(!strcmp(argv[1], "inspect"));
  }
  if (!strcmp(argv[1], "tls") || !strcmp(argv[1], "tls-inspect")) {
    return tls_lifecycle(!strcmp(argv[1], "tls-inspect"));
  }
  if (!strcmp(argv[1], "bind")) {
    return bind_initial();
  }
  if (!strcmp(argv[1], "refusal")) {
    return refusal();
  }
  if (!strcmp(argv[1], "exhaust")) {
    return exhaustion();
  }
  if (!strcmp(argv[1], "close")) {
    return close_race();
  }
  if (!strcmp(argv[1], "stop") || !strcmp(argv[1], "fault")) {
    return supervise(!strcmp(argv[1], "fault"), false, false);
  }
  if (!strcmp(argv[1], "tls-stop") || !strcmp(argv[1], "tls-fault")) {
    return supervise(!strcmp(argv[1], "tls-fault"), false, true);
  }
  if (!strcmp(argv[1], "park32") || !strcmp(argv[1], "park64") || !strcmp(argv[1], "park-inspect")) {
    return parking_contention(!strcmp(argv[1], "park32") ? THREAD_ADDRESS_WORD32_BYTES :
        THREAD_ADDRESS_WORD64_BYTES, !strcmp(argv[1], "park-inspect"));
  }
  if (!strcmp(argv[1], "park-refusal")) {
    return parking_refusal();
  }
  if (!strcmp(argv[1], "park-unmap")) {
    return parking_unmap();
  }
  if (!strcmp(argv[1], "park-stop")) {
    return supervise(false, true, false);
  }
  fputs("threadprobe: unknown mode\n", stderr);
  return 1;
}
