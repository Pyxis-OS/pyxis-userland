#include "memory_operations.h"
#include "runtime.h"
#include <abi/message.h>
#include <clock.h>
#include <errno.h>
#include <handle.h>
#include <memory.h>
#include <pthread.h>
#include <startup.h>
#include <environment.h>
#include <stdlib.h>
#include <thread.h>
#include <threads.h>
#include <wait.h>
#include "errors.h"
#include "thread_platform.h"
#include "thread_private.h"
#include "thread_sync.h"

/* Implementation limits, not a public ABI. The initial record occupies slot 0. */
#ifndef LIBC_THREAD_LIMIT
#define LIBC_THREAD_LIMIT 64
#endif
#define LIBC_THREAD_STACK_DEFAULT (1024 * 1024)
#define LIBC_THREAD_STACK_MIN (1024 * 1024)
#define LIBC_THREAD_STACK_MAX (8 * 1024 * 1024)

enum lifecycle_state { THREAD_FREE, THREAD_INITIAL, THREAD_PREPARING,
  THREAD_ACTIVE, THREAD_QUARANTINED };
enum entry_kind { ENTRY_PTHREAD, ENTRY_C11 };

struct lifecycle_record {
  uint32_t generation;
  enum lifecycle_state state;
  bool joining;
  bool detached;
  uint32_t published;
  enum entry_kind kind;
  union {
    void *(*pthread_start)(void *);
    thrd_start_t c11_start;
  } start;
  void *argument;
  struct memory_reservation stack;
  struct libc_thread_context context;
  handle_t completion;
};

static struct lifecycle_record records[LIBC_THREAD_LIMIT];
static struct libc_lock registry_lock;
static handle_t service, clock_read, clock_sleep, memory;
static bool creations_closed;
/* Records marked detached and not yet collected; lets the collection scan skip
 * the common case of none. Guarded by registry_lock. */
static size_t detached_live;

_Static_assert(LIBC_THREAD_LIMIT > 0 && LIBC_THREAD_LIMIT <= UINT32_MAX,
    "thread registry identity capacity");

extern void libc_thread_entry(void);
[[noreturn]] void libc_thread_run(void *argument);

static uint64_t identity(struct lifecycle_record *record)
{
  return ((uint64_t)record->generation << 32) | (uint32_t)(record - records + 1);
}

static handle_t retain_startup(const char *name, uint64_t protocol, uint64_t rights,
    uint64_t optional_rights)
{
  handle_t source;
  enum call_status status = environment_capability(startup_environment_handle(), name, &source);
  if (status == CALL_UNKNOWN_SYSTEM_CALL) {
    libc_start_failed(PROCESS_START_RESOURCES, status);
  }
  if (status != CALL_OK) {
    return HANDLE_INVALID;
  }
  struct handle_info info;
  handle_t retained = HANDLE_INVALID;
  status = handle_query(source, &info);
  if (status == CALL_UNKNOWN_SYSTEM_CALL) {
    libc_start_failed(PROCESS_START_RESOURCES, status);
  }
  if (status == CALL_OK && info.kind == HANDLE_KIND_NATIVE && info.protocol == protocol &&
      (info.rights & rights) == rights) {
    status = handle_copy_restricted(source,
        rights | (info.rights & optional_rights), 0, &retained);
    if (status == CALL_UNKNOWN_SYSTEM_CALL || status == CALL_OUTCOME_UNKNOWN) {
      libc_start_failed(PROCESS_START_RESOURCES, status);
    }
  }
  libc_startup_close(source, PROCESS_START_RESOURCES);
  return retained;
}

void libc_thread_runtime_init(void)
{
  records[0].generation = 1;
  records[0].state = THREAD_INITIAL;
  struct libc_thread_record *record = libc_thread_record();
  record->thread_identity = identity(&records[0]);
  memory = record->memory;
  service = retain_startup("threads", PROTOCOL_THREAD_SERVICE, THREAD_RIGHT_MANAGE, 0);
  clock_read = retain_startup("clock", PROTOCOL_CLOCK, CLOCK_RIGHT_READ, CLOCK_RIGHT_PRECISE);
  clock_sleep = retain_startup("clock", PROTOCOL_CLOCK, CLOCK_RIGHT_SLEEP, 0);
}

handle_t libc_thread_service(void)
{
  return service;
}

handle_t libc_thread_clock(void)
{
  return clock_read;
}

handle_t libc_thread_sleep_clock(void)
{
  return clock_sleep;
}

uint64_t libc_thread_self(void)
{
  return libc_thread_record()->thread_identity;
}

struct libc_once_frame *libc_thread_once_head(void)
{
  return libc_thread_record()->once_frames;
}

void libc_thread_once_push(struct libc_once_frame *frame)
{
  struct libc_thread_record *record = libc_thread_record();
  frame->previous = record->once_frames;
  record->once_frames = frame;
}

void libc_thread_once_pop(struct libc_once_frame *frame)
{
  struct libc_thread_record *record = libc_thread_record();
  if (record->once_frames != frame) {
    _Exit(EXIT_FAILURE);
  }
  record->once_frames = frame->previous;
}

static void collect_detached(void);

bool libc_thread_shutdown_ready(void)
{
  /* Orderly finalizers require the initial thread and collected workers. A
   * forced process exit cannot wait for a stopped lock owner to run callbacks. */
  bool ready = libc_thread_self() == identity(&records[0]);
  if (ready) {
    collect_detached();
  }
  libc_lock_acquire(&registry_lock);
  for (size_t index = 1; ready && index < LIBC_THREAD_LIMIT; index++) {
    ready = records[index].state == THREAD_FREE;
  }
  if (ready) {
    creations_closed = true;
  }
  libc_lock_release(&registry_lock);
  return ready;
}

static struct lifecycle_record *find_record(uint64_t value)
{
  uint32_t slot = value;
  uint32_t generation = value >> 32;
  if (!slot || slot > LIBC_THREAD_LIMIT || !generation) {
    return NULL;
  }
  struct lifecycle_record *record = &records[slot - 1];
  return record->state != THREAD_FREE && record->generation == generation ? record : NULL;
}

static struct lifecycle_record *reserve_record(void)
{
  struct lifecycle_record *chosen = NULL;
  libc_lock_acquire(&registry_lock);
  for (size_t index = 1; !creations_closed && index < LIBC_THREAD_LIMIT; index++) {
    struct lifecycle_record *record = &records[index];
    if (record->state == THREAD_FREE && record->generation != UINT32_MAX) {
      uint32_t generation = record->generation + 1;
      *record = (struct lifecycle_record){
        .generation = generation, .state = THREAD_PREPARING,
      };
      chosen = record;
      break;
    }
  }
  libc_lock_release(&registry_lock);
  return chosen;
}

static int release_record(struct lifecycle_record *record)
{
  int error = 0;
  if (record->context.reservation.identity) {
    enum call_status status = libc_thread_context_release(memory, &record->context);
    if (status != CALL_OK) {
      error = libc_call_errno(status);
    }
  }
  if (record->stack.identity) {
    enum call_status status = libc_memory_release_reservation(memory, record->stack.identity);
    if (status == CALL_OK) {
      record->stack = (struct memory_reservation){0};
    } else if (!error) {
      error = libc_call_errno(status);
    }
  }
  if (record->completion != HANDLE_INVALID) {
    handle_t completion = record->completion;
    record->completion = HANDLE_INVALID;
    if (handle_close(completion) != 0 && !error) {
      error = EIO;
    }
  }
  libc_lock_acquire(&registry_lock);
  /* A failed/uncertain cleanup consumes capacity until process teardown. Do
   * not reuse an identity or retry an operation with an unknown effect. */
  record->state = error ? THREAD_QUARANTINED : THREAD_FREE;
  record->joining = false;
  if (record->detached) {
    record->detached = false;
    --detached_live;
  }
  libc_lock_release(&registry_lock);
  return error;
}

/* A detached thread has no joiner, so its record, stack and observer are
 * collected here instead, once its native completion is ready. The exiting
 * thread never frees its own stack; the next create, join or orderly exit does.
 * Called without registry_lock. The joining flag is the exclusive claim, as for
 * join, so a record is collected at most once. */
static void collect_detached(void)
{
  libc_lock_acquire(&registry_lock);
  bool any = detached_live != 0;
  libc_lock_release(&registry_lock);
  if (!any) {
    return;
  }
  for (size_t index = 1; index < LIBC_THREAD_LIMIT; index++) {
    struct lifecycle_record *record = &records[index];
    libc_lock_acquire(&registry_lock);
    bool claimed = record->state == THREAD_ACTIVE && record->detached && !record->joining;
    handle_t completion = claimed ? record->completion : HANDLE_INVALID;
    if (claimed) {
      record->joining = true;
    }
    libc_lock_release(&registry_lock);
    if (!claimed) {
      continue;
    }
    struct wait_interest interest = {.handle = completion, .events = WAIT_COMPLETE};
    uint64_t events = 0;
    struct thread_result result;
    if (wait_many(&interest, 1, 0, &events) == CALL_OK && (events & WAIT_COMPLETE) &&
        thread_wait(completion, &result) == CALL_OK) {
      release_record(record);
      continue;
    }
    libc_lock_acquire(&registry_lock);
    record->joining = false;
    libc_lock_release(&registry_lock);
  }
}

static int create_record(uint64_t *thread, size_t stack_size, enum entry_kind kind,
    void *(*pthread_start)(void *), thrd_start_t c11_start, void *argument)
{
  if (!thread || (!pthread_start && !c11_start) ||
      stack_size < LIBC_THREAD_STACK_MIN || stack_size > LIBC_THREAD_STACK_MAX ||
      stack_size % MEMORY_PAGE_SIZE) {
    return EINVAL;
  }
  if (service == HANDLE_INVALID || memory == HANDLE_INVALID || !libc_thread_self()) {
    return EPERM;
  }
  collect_detached();
  struct lifecycle_record *record = reserve_record();
  if (!record) {
    return EAGAIN;
  }
  record->kind = kind;
  if (kind == ENTRY_PTHREAD) {
    record->start.pthread_start = pthread_start;
  } else {
    record->start.c11_start = c11_start;
  }
  record->argument = argument;
  enum call_status status = libc_memory_allocate_guarded(memory, stack_size, &record->stack);
  if (status == CALL_OK) {
    status = libc_thread_context_prepare(memory, &record->context);
  }
  if (status == CALL_OK) {
    struct libc_thread_record *context = (void *)record->context.fs_base;
    context->thread_identity = identity(record);
    struct thread_create request = {
      .entry = (uintptr_t)libc_thread_entry,
      .argument = (uintptr_t)record,
      .stack_identity = record->stack.identity,
      .context_identity = record->context.reservation.identity,
      .fs_base = record->context.fs_base,
    };
    status = libc_native_thread_create(service, &request, &record->completion);
  }
  if (status == CALL_OUTCOME_UNKNOWN) {
    /* The sibling may own these pins already. Only process teardown may
     * reclaim them without a trustworthy completion observer. */
    _Exit(EXIT_FAILURE);
  }
  if (status != CALL_OK) {
    int error = status == CALL_LIMIT ? EAGAIN : libc_call_errno(status);
    release_record(record);
    return error;
  }
  libc_lock_acquire(&registry_lock);
  record->state = THREAD_ACTIVE;
  /* Publication can run the child immediately. Exclude join/reuse until the
   * creator has finished accessing this record and its bounded wake word. */
  *thread = identity(record);
  __atomic_store_n(&record->published, 1, __ATOMIC_RELEASE);
  if (libc_wake(&record->published, UINT64_MAX)) {
    _Exit(EXIT_FAILURE);
  }
  libc_lock_release(&registry_lock);
  return 0;
}

[[noreturn]] void libc_thread_run(void *argument)
{
  struct lifecycle_record *record = argument;
  /* Native CREATE can dispatch before its caller receives the observer. No
   * user callback publishes a joinable identity until that observer is ready. */
  while (!__atomic_load_n(&record->published, __ATOMIC_ACQUIRE)) {
    if (libc_park(&record->published, 0, THREAD_ADDRESS_NO_DEADLINE)) {
      _Exit(EXIT_FAILURE);
    }
  }
  uint64_t value;
  if (record->kind == ENTRY_PTHREAD) {
    value = (uintptr_t)record->start.pthread_start(record->argument);
  } else {
    value = (uint64_t)(int64_t)record->start.c11_start(record->argument);
  }
  libc_thread_exit(service, value);
}

static int join_record(uint64_t value, enum entry_kind kind, uint64_t *result)
{
  if (value == libc_thread_self()) {
    return EDEADLK;
  }
  collect_detached();
  libc_lock_acquire(&registry_lock);
  struct lifecycle_record *record = find_record(value);
  if (!record) {
    libc_lock_release(&registry_lock);
    return ESRCH;
  }
  if (record == &records[0] || record->state != THREAD_ACTIVE ||
      record->kind != kind || record->joining || record->detached) {
    libc_lock_release(&registry_lock);
    return EINVAL;
  }
  record->joining = true;
  libc_lock_release(&registry_lock);
  struct thread_result completion;
  enum call_status status = thread_wait(record->completion, &completion);
  if (status != CALL_OK) {
    libc_lock_acquire(&registry_lock);
    record->joining = false;
    libc_lock_release(&registry_lock);
    return libc_call_errno(status);
  }
  uint64_t exit_value = completion.value;
  int error = release_record(record);
  if (error) {
    return error;
  }
  if (completion.kind != THREAD_EXITED) {
    return ECANCELED;
  }
  if (result) {
    *result = exit_value;
  }
  return 0;
}

/* A thread may detach itself. Its record stays ACTIVE until collected, so the
 * identity is valid, and refused as a join target, until then. */
static int detach_record(uint64_t value, enum entry_kind kind)
{
  libc_lock_acquire(&registry_lock);
  struct lifecycle_record *record = find_record(value);
  if (!record) {
    libc_lock_release(&registry_lock);
    return ESRCH;
  }
  if (record == &records[0] || record->state != THREAD_ACTIVE ||
      record->kind != kind || record->joining || record->detached) {
    libc_lock_release(&registry_lock);
    return EINVAL;
  }
  record->detached = true;
  ++detached_live;
  libc_lock_release(&registry_lock);
  return 0;
}

int pthread_attr_init(pthread_attr_t *attributes)
{
  if (!attributes) {
    return EINVAL;
  }
  *attributes = (pthread_attr_t){LIBC_THREAD_STACK_DEFAULT, PTHREAD_CREATE_JOINABLE};
  return 0;
}

int pthread_attr_destroy(pthread_attr_t *attributes)
{
  return attributes ? 0 : EINVAL;
}

int pthread_attr_setstacksize(pthread_attr_t *attributes, size_t size)
{
  if (!attributes || size < LIBC_THREAD_STACK_MIN || size > LIBC_THREAD_STACK_MAX ||
      size % MEMORY_PAGE_SIZE) {
    return EINVAL;
  }
  attributes->stack_size = size;
  return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attributes, size_t *size)
{
  if (!attributes || !size) {
    return EINVAL;
  }
  *size = attributes->stack_size;
  return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attributes, int state)
{
  if (!attributes || (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
    return EINVAL;
  }
  attributes->detach_state = state;
  return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attributes, int *state)
{
  if (!attributes || !state) {
    return EINVAL;
  }
  *state = attributes->detach_state;
  return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
    void *(*start)(void *), void *argument)
{
  int error = create_record(thread, attributes ? attributes->stack_size :
      LIBC_THREAD_STACK_DEFAULT, ENTRY_PTHREAD, start, NULL, argument);
  if (!error && attributes && attributes->detach_state == PTHREAD_CREATE_DETACHED) {
    /* The child may already have finished; its record outlives that. */
    error = detach_record(*thread, ENTRY_PTHREAD);
  }
  return error;
}

int pthread_join(pthread_t thread, void **result)
{
  uint64_t value;
  int error = join_record(thread, ENTRY_PTHREAD, &value);
  if (!error && result) {
    *result = (void *)(uintptr_t)value;
  }
  return error;
}

int pthread_detach(pthread_t thread)
{
  return detach_record(thread, ENTRY_PTHREAD);
}

pthread_t pthread_self(void)
{
  return libc_thread_self();
}

int pthread_equal(pthread_t left, pthread_t right)
{
  return left != 0 && left == right;
}

[[noreturn]] void pthread_exit(void *result)
{
  if (service == HANDLE_INVALID || !libc_thread_self()) {
    _Exit(EXIT_FAILURE);
  }
  libc_thread_exit(service, (uintptr_t)result);
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
  return libc_call_errno(libc_thread_key_create(destructor, key));
}

int pthread_key_delete(pthread_key_t key)
{
  return libc_call_errno(libc_thread_key_delete(key));
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
  return libc_call_errno(libc_thread_key_set(key, (void *)value));
}

void *pthread_getspecific(pthread_key_t key)
{
  void *value = NULL;
  libc_thread_key_get(key, &value);
  return value;
}

static int c11_error(int error)
{
  return error == 0 ? thrd_success : error == ENOMEM ? thrd_nomem : thrd_error;
}

int thrd_create(thrd_t *thread, thrd_start_t start, void *argument)
{
  return c11_error(create_record(thread, LIBC_THREAD_STACK_DEFAULT,
      ENTRY_C11, NULL, start, argument));
}

int thrd_join(thrd_t thread, int *result)
{
  uint64_t value;
  int error = join_record(thread, ENTRY_C11, &value);
  if (!error && result) {
    *result = (int)(int64_t)value;
  }
  return c11_error(error);
}

int thrd_detach(thrd_t thread)
{
  return c11_error(detach_record(thread, ENTRY_C11));
}

thrd_t thrd_current(void)
{
  return libc_thread_self();
}

int thrd_equal(thrd_t left, thrd_t right)
{
  return pthread_equal(left, right);
}

[[noreturn]] void thrd_exit(int result)
{
  pthread_exit((void *)(uintptr_t)(int64_t)result);
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
  if (!duration || duration->tv_sec < 0 || duration->tv_nsec < 0 ||
      duration->tv_nsec >= 1000000000L || !clock_read || !clock_sleep) {
    return -2;
  }
  uint64_t now;
  if (clock_now(clock_read, &now) != CALL_OK) {
    return -2;
  }
  uint64_t seconds = duration->tv_sec;
  if (now == UINT64_MAX ||
      seconds > (UINT64_MAX - now - 1) / UINT64_C(1000000000)) {
    return -2;
  }
  uint64_t deadline = now + seconds * UINT64_C(1000000000);
  if ((uint64_t)duration->tv_nsec >= UINT64_MAX - deadline) {
    return -2;
  }
  deadline += duration->tv_nsec;
  if (clock_sleep_until(clock_sleep, deadline) != CALL_OK) {
    return -2;
  }
  if (remaining) {
    *remaining = (struct timespec){0};
  }
  return 0;
}

int tss_create(tss_t *key, tss_dtor_t destructor)
{
  return c11_error(pthread_key_create(key, destructor));
}

void tss_delete(tss_t key)
{
  pthread_key_delete(key);
}

int tss_set(tss_t key, void *value)
{
  return c11_error(pthread_setspecific(key, value));
}

void *tss_get(tss_t key)
{
  return pthread_getspecific(key);
}
