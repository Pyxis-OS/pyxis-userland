#include <environment.h>
#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <console.h>
#include <clock.h>
#include <transaction.h>
#include <resource.h>
#include <pyxis/resource.h>
#include <pyxis/working_path.h>
#include <stream.h>
#include <directory.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <pipe.h>
#include <output.h>
#include <path.h>
#include <pyxis/descriptor.h>
#include <startup.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include "descriptor.h"
#include "errors.h"
#include "stream.h"
#include "thread_sync.h"
#include "thread_private.h"
#include "thread_platform.h"

enum descriptor_state { DESCRIPTOR_FREE, DESCRIPTOR_RESERVED, DESCRIPTOR_OPEN };
enum descriptor_kind { DESCRIPTOR_CONSOLE, DESCRIPTOR_FILE, DESCRIPTOR_PIPE, DESCRIPTOR_RESPONSE };

/* Each descriptor owns one reference to an open object. Duplication shares its
 * native handle, access, append policy, logical position and unread read-ahead.
 * Position is the next byte delivered; backend reads start after read-ahead. */
struct descriptor_transaction {
  handle_t control, body, response;
  uint64_t deadline;
  bool inplace, attempted, committed, producer_failed;
  int failure;
  uint64_t failure_outcome;
  bool result_failed;
  struct describe_reply failed_result;
};

struct descriptor_object {
  size_t references; /* Descriptor aliases, protected by table_guard. */
  size_t storage; /* Aliases and admitted operations; no native grant copies. */
  bool closed;
  struct libc_mutex read_guard, write_guard;
  bool allocated;
  enum descriptor_kind kind;
  handle_t handle;
  struct descriptor_transaction *transaction;
  bool final_checked;
  int final_error;
  uint64_t position;
  bool readable, writable, append;
  bool input_consumed; /* Any successful read, including explicit-offset reads. */
  bool unbuffered; /* Read-ahead allocation failed; stay exact until close. */
  unsigned char *ahead; /* Owned, BUFSIZ bytes, allocated on first fill. */
  size_t ahead_start, ahead_count;
  /* Owned pipe value being read through as bytes, and the next offset. */
  handle_t value;
  uint64_t value_offset;
};

struct descriptor_entry {
  uint64_t generation;
  enum descriptor_state state;
  struct descriptor_object *object;
  FILE *stream; /* Non-owning; only one FILE can associate with this slot. */
  uint64_t environment_identity;
  enum environment_stream_index original_stream;
};

static struct descriptor_object startup_objects[ENVIRONMENT_STREAM_COUNT];
static struct descriptor_entry startup_entries[ENVIRONMENT_STREAM_COUNT];
static struct descriptor_entry *entries = startup_entries;
static size_t capacity = ENVIRONMENT_STREAM_COUNT;
static struct libc_lock table_guard;

static int commit_locked(struct descriptor_object *object);

static int fail(int error)
{
  errno = error;
  return -1;
}

static void unlock_object(struct libc_mutex *mutex)
{
  if (libc_mutex_unlock(mutex)) {
    __builtin_trap();
  }
}

static void install_locked(int descriptor, struct descriptor_entry entry)
{
  entry.generation = entries[descriptor].generation + 1;
  entries[descriptor] = entry;
}

static struct descriptor_object *lookup(int descriptor)
{
  if (descriptor < 0 || (size_t)descriptor >= capacity ||
      entries[descriptor].state != DESCRIPTOR_OPEN) {
    errno = EBADF;
    return NULL;
  }
  return entries[descriptor].object;
}

/* Allocation and native operations never run with table_guard held. */
static int grow_entries(size_t required)
{
  size_t limit = (size_t)INT_MAX + 1;
  if (limit > SIZE_MAX / sizeof(*entries)) {
    limit = SIZE_MAX / sizeof(*entries);
  }
  if (required > limit) {
    return fail(EMFILE);
  }
  for (;;) {
    libc_lock_acquire(&table_guard);
    size_t current = capacity;
    libc_lock_release(&table_guard);
    if (required <= current) {
      return 0;
    }
    size_t next = current;
    while (next < required) {
      next = next > limit / 2 ? limit : next * 2;
    }
    struct descriptor_entry *grown = calloc(next, sizeof(*grown));
    if (!grown) {
      return -1;
    }
    libc_lock_acquire(&table_guard);
    if (capacity != current) {
      libc_lock_release(&table_guard);
      free(grown);
      continue;
    }
    struct descriptor_entry *previous = entries;
    memcpy(grown, entries, current * sizeof(*entries));
    entries = grown;
    capacity = next;
    libc_lock_release(&table_guard);
    if (previous != startup_entries) {
      free(previous);
    }
    return 0;
  }
}

static int reserve(void)
{
  for (;;) {
    libc_lock_acquire(&table_guard);
    for (size_t index = 0; index < capacity; ++index) {
      if (entries[index].state == DESCRIPTOR_FREE) {
        entries[index].state = DESCRIPTOR_RESERVED;
        ++entries[index].generation;
        libc_lock_release(&table_guard);
        return (int)index;
      }
    }
    size_t required = capacity + 1;
    libc_lock_release(&table_guard);
    if (grow_entries(required) < 0) {
      return -1;
    }
  }
}

static void publish(int descriptor, struct descriptor_entry entry)
{
  libc_lock_acquire(&table_guard);
  install_locked(descriptor, entry);
  if (entry.state == DESCRIPTOR_OPEN && entry.stream) {
    __atomic_store_n(&entry.stream->descriptor, descriptor, __ATOMIC_RELEASE);
  }
  libc_lock_release(&table_guard);
}

static bool retain_storage(struct descriptor_object *object)
{
  size_t count = __atomic_load_n(&object->storage, __ATOMIC_RELAXED);
  while (count != SIZE_MAX && !__atomic_compare_exchange_n(&object->storage,
      &count, count + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
  return count != SIZE_MAX;
}

struct descriptor_object *descriptor_hold(int descriptor)
{
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  if (object) {
    if (!retain_storage(object)) {
      object = NULL;
      errno = EMFILE;
    }
  }
  libc_lock_release(&table_guard);
  return object;
}

void descriptor_drop(struct descriptor_object *object)
{
  if (!object || __atomic_sub_fetch(&object->storage, 1, __ATOMIC_ACQ_REL)) {
    return;
  }
  free(object->ahead);
  free(object->transaction);
  if (object->allocated) {
    free(object);
  }
}

static int object_ready(struct descriptor_object *object, bool writing)
{
  if (!object || __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE) ||
      (writing ? !__atomic_load_n(&object->writable, __ATOMIC_ACQUIRE) :
      !__atomic_load_n(&object->readable, __ATOMIC_ACQUIRE))) {
    return fail(EBADF);
  }
  if (!writing && object->transaction) {
    int error = __atomic_load_n(&object->transaction->failure, __ATOMIC_ACQUIRE);
    if (error || !__atomic_load_n(&object->transaction->committed, __ATOMIC_ACQUIRE)) {
      return fail(error ? error : EBUSY);
    }
  }
  return 0;
}

int descriptor_release_handle(handle_t handle)
{
  struct syscall_result result = syscall_close(handle);
  if (result.status >= CALL_STATUS_COUNT || result.reply_size) {
    return fail(EIO);
  }
  if (result.status != CALL_OK) {
    return fail(libc_call_errno(result.status));
  }
  return 0;
}

void descriptor_adopt_standard(enum environment_stream_index index, FILE *stream,
    struct pyxis_stdio_binding binding, uint64_t identity)
{
  if (binding.protocol == 0) {
    return;
  }
  /* Adopt the owned lookup grants; the initialization snapshot is released
   * before constructors or main can observe endpoint lifetime. */
  struct descriptor_object *object = &startup_objects[index];
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1,
    .kind = binding.protocol == PROTOCOL_FILE ? DESCRIPTOR_FILE :
            binding.protocol == PROTOCOL_PIPE ? DESCRIPTOR_PIPE :
            binding.protocol == PROTOCOL_STREAM ? DESCRIPTOR_RESPONSE : DESCRIPTOR_CONSOLE,
    .handle = binding.handle,
    .readable = index == ENVIRONMENT_STDIN,
    .writable = index != ENVIRONMENT_STDIN, .append = binding.append,
  };
  entries[index] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
    .environment_identity = identity,
    .original_stream = index,
  };
  stream->descriptor = (int)index;
}

static int resource_errno(uint64_t outcome)
{
  return libc_call_errno(resource_outcome_status(outcome));
}

static bool named_path(const char *path)
{
  if (!path) {
    return false;
  }
  const char *end = path;
  while (*end && *end != '/' && *end != ':') {
    ++end;
  }
  return *end == ':' && end[1] == '/' && end[2] == '/';
}

static enum call_status open_typed_path(const struct path_context *context,
    const char *path, uint64_t intent, uint64_t flags, struct resource_result *result)
{
  if (!path) {
    return CALL_BAD_REQUEST;
  }
  const char *end = path;
  while (*end && *end != '/' && *end != ':') {
    ++end;
  }
  if (*end != ':' || end[1] != '/' || end[2] != '/') {
    return resource_open_path(context, path, intent, flags, 0, result);
  }
  size_t size = end - path;
  if (!size || size > ENVIRONMENT_NAME_MAX) {
    return CALL_BAD_REQUEST;
  }
  char name[ENVIRONMENT_NAME_MAX + 1];
  memcpy(name, path, size);
  name[size] = 0;
  handle_t target = HANDLE_INVALID;
  enum call_status status = path_root(name, &target);
  struct handle_info info;
  if (status == CALL_OK) {
    status = handle_query(target, &info);
  }
  if (status == CALL_OK && info.protocol != PROTOCOL_DIRECTORY &&
      info.protocol != PROTOCOL_PROVIDER && !end[3]) {
    /* A held FILE already exists: w may truncate it without inventing CREATE
     * authority or changing the native held-OPEN contract. */
    if (flags & PROVIDER_OPEN_EXCLUSIVE) {
      status = CALL_ALREADY_EXISTS;
    } else {
      flags &= ~PROVIDER_OPEN_CREATE;
      if (info.protocol != PROTOCOL_FILE) {
        flags &= ~PROVIDER_OPEN_TRUNCATE;
      }
    }
  }
  if (status == CALL_OK) {
    status = resource_open_held(target, end + 3, intent, flags, 0, result);
  }
  if (target) {
    handle_close(target);
  }
  return status;
}

static int open_common(const char *path, const struct descriptor_mode *mode, FILE *stream,
    struct pyxis_open_result *failure)
{
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  /* Worst-case storage is reserved before a typed OPEN may create/truncate. */
  struct descriptor_object *object = calloc(1, sizeof(*object));
  struct descriptor_transaction *transaction = calloc(1, sizeof(*transaction));
  if (!object || !transaction) {
    free(object);
    free(transaction);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(ENOMEM);
  }
  struct resource_result opened = {0};
  enum call_status status;
  {
    struct pyxis_working_snapshot working = {0};
    /* Absolute names capture their environment entry, not the working chain. */
    status = named_path(path) ? CALL_OK : pyxis_working_snapshot_init(&working, NULL);
    if (status == CALL_OK) {
      uint64_t intent = (mode->readable ? PROVIDER_INTENT_READ : 0) |
          (mode->writable ? PROVIDER_INTENT_WRITE : 0);
      uint64_t flags = (mode->create ? PROVIDER_OPEN_CREATE : 0) |
          (mode->truncate ? PROVIDER_OPEN_TRUNCATE : 0) |
          (mode->append ? PROVIDER_OPEN_APPEND : 0) |
          (mode->exclusive ? PROVIDER_OPEN_EXCLUSIVE : 0);
      status = open_typed_path(&working.context, path, intent, flags, &opened);
    }
    pyxis_working_snapshot_close(&working);
  }
  int error = status == CALL_OK ? resource_errno(opened.info.description.outcome) :
      status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ? EIO :
      libc_path_errno(status, path, NULL);
  if (status != CALL_OK && failure) {
    uint64_t outcome = status == CALL_NOT_FOUND ? RESOURCE_NOT_FOUND :
        status == CALL_DENIED ? RESOURCE_DENIED : status == CALL_OUTCOME_UNKNOWN ? RESOURCE_UNCERTAIN :
        status == CALL_ABANDONED ? RESOURCE_ABORTED :
        status == CALL_FILE_TOO_LARGE ? RESOURCE_TOO_LARGE :
        status == CALL_IO || status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ? RESOURCE_IO_ERROR : RESOURCE_OK;
    if (outcome != RESOURCE_OK) {
      failure->available = true;
      failure->resource.info.description.outcome = outcome;
    }
  }
  enum descriptor_kind kind = DESCRIPTOR_FILE;
  uint64_t position = 0;
  bool transactional = status == CALL_OK && opened.info.protocol == PROTOCOL_TRANSACTION;
  if (!error) {
    uint64_t protocol = opened.info.protocol;
    if (protocol == PROTOCOL_FILE) {
      kind = DESCRIPTOR_FILE;
    } else if (protocol == PROTOCOL_STREAM) {
      kind = DESCRIPTOR_RESPONSE;
    } else if (protocol == PROTOCOL_PIPE && opened.info.kind == HANDLE_KIND_NATIVE) {
      kind = DESCRIPTOR_PIPE;
    } else if (protocol == PROTOCOL_CONSOLE && opened.info.kind == HANDLE_KIND_NATIVE) {
      kind = DESCRIPTOR_CONSOLE;
    } else if (!transactional) {
      error = protocol == PROTOCOL_DIRECTORY ? EISDIR : ENOTSUP;
    }
    if (!error && !transactional) {
      if ((kind == DESCRIPTOR_RESPONSE && mode->writable) ||
          (kind != DESCRIPTOR_FILE && mode->append)) {
        error = ENOTSUP;
      }
      uint64_t read_right = kind == DESCRIPTOR_PIPE ? PIPE_RIGHT_READ :
          kind == DESCRIPTOR_CONSOLE ? CONSOLE_RIGHT_READ :
          kind == DESCRIPTOR_RESPONSE ? STREAM_RIGHT_READ : FILE_RIGHT_READ;
      uint64_t write_right = kind == DESCRIPTOR_PIPE ? PIPE_RIGHT_WRITE :
          kind == DESCRIPTOR_CONSOLE ? CONSOLE_RIGHT_WRITE : FILE_RIGHT_WRITE;
      uint64_t needed = (mode->readable ? read_right : 0) | (mode->writable ? write_right : 0);
      if (!error && (opened.info.rights & needed) != needed) {
        error = EACCES;
      }
    } else if (!error && (opened.info.kind != HANDLE_KIND_EXPORTED ||
        !(opened.info.transport & HANDLE_TRANSPORT_CALL) || mode->append || mode->exclusive ||
        !mode->writable)) {
      error = ENOTSUP;
    } else if (!error && (!(opened.info.rights & TRANSACTION_RIGHT_COMMIT) ||
        (mode->writable && !(opened.info.rights & TRANSACTION_RIGHT_STAGE)))) {
      error = EACCES;
    }
  }
  if (!error && transactional) {
    uint64_t now;
    status = clock_now(libc_thread_clock(), &now);
    if (status == CALL_OK && now > UINT64_MAX - UINT64_C(30000000000)) {
      status = CALL_LIMIT;
    }
    if (status == CALL_OK) {
      transaction->deadline = now + UINT64_C(30000000000);
      transaction->control = opened.resource;
      status = transaction_stage(opened.resource, transaction->deadline, &transaction->body);
    }
    if (status == CALL_OK && mode->truncate) {
      status = file_resize(transaction->body, 0);
    }
    error = status == CALL_OK ? 0 : status == CALL_TIMED_OUT ||
        status == CALL_ENDPOINT_CLOSED ? EIO : libc_call_errno(status);
  }
  if (!error && !transactional && kind == DESCRIPTOR_FILE && mode->append && !mode->readable) {
    status = file_size(opened.resource, &position);
    error = status == CALL_OK ? 0 : libc_call_errno(status);
  }
  if (error) {
    if ((status == CALL_OK || opened.resource != HANDLE_INVALID) && failure) {
      failure->available = true;
      failure->resource = opened;
      opened = (struct resource_result){0};
      if (transaction->control == failure->resource.resource) {
        transaction->control = 0;
      }
    }
    if (transaction->control == opened.resource) {
      transaction->control = HANDLE_INVALID;
    }
    resource_result_close(&opened);
    if (transaction->control) {
      handle_close(transaction->control);
    }
    if (transaction->body) {
      handle_close(transaction->body);
    }
    free(transaction);
    free(object);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(error);
  }
  transaction->inplace = mode->readable;
  handle_t handle = transactional ? transaction->body : opened.resource;
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1, .allocated = true, .kind = kind, .handle = handle,
    .transaction = transactional ? transaction : NULL,
    .position = position, .readable = mode->readable, .writable = mode->writable,
    .append = mode->append,
  };
  opened.resource = HANDLE_INVALID;
  resource_result_close(&opened);
  if (!object->transaction) {
    free(transaction);
  }
  publish(descriptor, (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  });
  return descriptor;
}

int descriptor_open_result(const char *path, const struct descriptor_mode *mode,
    FILE *stream, struct pyxis_open_result *failure)
{
  return open_common(path, mode, stream, failure);
}

int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream)
{
  return descriptor_open_result(path, mode, stream, NULL);
}

int descriptor_tmpfile(FILE *stream, handle_t parent)
{
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  struct descriptor_object *object = malloc(sizeof(*object));
  if (!object) {
    publish(descriptor, (struct descriptor_entry){0});
    return -1;
  }
  char name[] = "tmp_XXXXXX";
  handle_t handle = HANDLE_INVALID;
  enum call_status status = CALL_ALREADY_EXISTS;
  for (int attempt = 0; attempt < TEMPORARY_CREATE_ATTEMPTS; ++attempt) {
    status = file_temporary_name(name + sizeof(name) - 1 - TEMPORARY_NAME_LENGTH);
    if (status != CALL_OK) {
      break;
    }
    status = directory_create(parent, name, DIRECTORY_KIND_FILE,
        FILE_RIGHT_READ | FILE_RIGHT_WRITE, &handle);
    if (status != CALL_ALREADY_EXISTS) {
      break;
    }
  }
  if (status == CALL_OK) {
    /* No allocation or path re-resolution after mutation: the original parent
     * is held throughout creation/removal, and the open file survives removal. */
    status = directory_remove(parent, name, DIRECTORY_KIND_FILE);
  }
  if (status != CALL_OK) {
    int error = libc_call_errno(status);
    if (handle != HANDLE_INVALID) {
      descriptor_release_handle(handle);
    }
    free(object);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(error);
  }
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1, .allocated = true, .kind = DESCRIPTOR_FILE, .handle = handle,
    .readable = true, .writable = true,
  };
  publish(descriptor, (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  });
  return descriptor;
}

int descriptor_stream(FILE *stream, struct pyxis_stdio_binding *binding)
{
  libc_lock_acquire(&table_guard);
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  struct descriptor_object *object = lookup(descriptor);
  int error = !object || entries[descriptor].stream != stream ? EBADF :
      object->transaction && !object->transaction->committed && object->kind == DESCRIPTOR_RESPONSE ? EBUSY : 0;
  if (!error) {
    *binding = (struct pyxis_stdio_binding){
      .protocol = object->kind == DESCRIPTOR_FILE ? PROTOCOL_FILE :
          object->kind == DESCRIPTOR_PIPE ? PROTOCOL_PIPE :
          object->kind == DESCRIPTOR_RESPONSE ? PROTOCOL_STREAM : PROTOCOL_CONSOLE,
      .handle = object->handle,
      .append = __atomic_load_n(&object->append, __ATOMIC_ACQUIRE),
    };
  }
  libc_lock_release(&table_guard);
  return error ? fail(error) : 0;
}

int pyxis_descriptor_borrow(int descriptor, struct pyxis_descriptor_binding *binding)
{
  if (!binding) {
    return fail(EINVAL);
  }
  *binding = (struct pyxis_descriptor_binding){0};
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  int error = libc_mutex_lock(&object->read_guard);
  if (error) {
    descriptor_drop(object);
    return fail(error);
  }
  struct pyxis_descriptor_binding result = {
    .handle = object->handle,
    .access = (object->readable && (!object->transaction || object->transaction->committed) ? PYXIS_DESCRIPTOR_READ : 0) |
        (object->writable ? PYXIS_DESCRIPTOR_WRITE : 0),
    /* A value being read through is available without waiting on the pipe. */
    .buffered_read = object->ahead_count + (object->value != HANDLE_INVALID),
    .append = __atomic_load_n(&object->append, __ATOMIC_ACQUIRE),
  };
  unlock_object(&object->read_guard);
  enum call_status status = handle_query(result.handle, &result.info);
  descriptor_drop(object);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  *binding = result;
  return 0;
}

int pyxis_descriptor_adopt(handle_t *handle, unsigned int access)
{
  if (!handle || *handle == HANDLE_INVALID || !access ||
      (access & ~(PYXIS_DESCRIPTOR_READ | PYXIS_DESCRIPTOR_WRITE))) {
    return fail(EINVAL);
  }
  libc_lock_acquire(&table_guard);
  for (size_t i = 0; i < capacity; ++i) {
    if (entries[i].state == DESCRIPTOR_OPEN && entries[i].object->handle == *handle) {
      libc_lock_release(&table_guard);
      return fail(EBUSY);
    }
  }
  libc_lock_release(&table_guard);
  struct handle_info info;
  enum call_status status = handle_query(*handle, &info);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  enum descriptor_kind kind;
  uint64_t read_right, write_right;
  if (info.protocol == PROTOCOL_FILE) {
    kind = DESCRIPTOR_FILE;
    read_right = FILE_RIGHT_READ;
    write_right = FILE_RIGHT_WRITE;
  } else if (info.protocol == PROTOCOL_PIPE && info.kind == HANDLE_KIND_NATIVE) {
    kind = DESCRIPTOR_PIPE;
    read_right = PIPE_RIGHT_READ;
    write_right = PIPE_RIGHT_WRITE;
  } else if (info.protocol == PROTOCOL_CONSOLE && info.kind == HANDLE_KIND_NATIVE) {
    kind = DESCRIPTOR_CONSOLE;
    read_right = CONSOLE_RIGHT_READ;
    write_right = CONSOLE_RIGHT_WRITE;
  } else {
    return fail(ENOTSUP);
  }
  uint64_t rights = (access & PYXIS_DESCRIPTOR_READ ? read_right : 0) |
      (access & PYXIS_DESCRIPTOR_WRITE ? write_right : 0);
  if ((info.rights & rights) != rights ||
      (info.kind == HANDLE_KIND_EXPORTED &&
       (info.transport & HANDLE_TRANSPORT_CALL) != HANDLE_TRANSPORT_CALL)) {
    return fail(EACCES);
  }
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  struct descriptor_object *object = malloc(sizeof(*object));
  if (!object) {
    publish(descriptor, (struct descriptor_entry){0});
    return -1;
  }
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1, .allocated = true, .kind = kind, .handle = *handle,
    .readable = access & PYXIS_DESCRIPTOR_READ,
    .writable = access & PYXIS_DESCRIPTOR_WRITE,
  };
  publish(descriptor, (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object,
  });
  *handle = HANDLE_INVALID;
  return descriptor;
}

bool descriptor_ready(int descriptor, bool writing)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  bool ready = object_ready(object, writing) == 0;
  descriptor_drop(object);
  return ready;
}

int descriptor_associate(int descriptor, const struct descriptor_mode *mode, FILE *stream)
{
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  int error = !object ? EBADF :
      (mode->readable && !object->readable) || (mode->writable && !object->writable) ?
      EBADF : entries[descriptor].stream ? EBUSY : 0;
  if (!error) {
    if (mode->append) {
      __atomic_store_n(&object->append, true, __ATOMIC_RELEASE);
    }
    entries[descriptor].stream = stream;
    ++entries[descriptor].generation;
    __atomic_store_n(&stream->descriptor, descriptor, __ATOMIC_RELEASE);
  }
  libc_lock_release(&table_guard);
  if (error) {
    return fail(error);
  }
  return 0;
}

int dup(int descriptor)
{
  struct descriptor_object *held = descriptor_hold(descriptor);
  if (!held) {
    return -1;
  }
  int copy = reserve();
  if (copy < 0) {
    int error = errno;
    descriptor_drop(held);
    return fail(error);
  }
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  int error = !object || object != held ? EBADF : object->references == SIZE_MAX ||
      __atomic_load_n(&object->storage, __ATOMIC_RELAXED) == SIZE_MAX ? EMFILE : 0;
  if (!error) {
    ++object->references;
    __atomic_add_fetch(&object->storage, 1, __ATOMIC_RELAXED);
    install_locked(copy, (struct descriptor_entry){.state = DESCRIPTOR_OPEN, .object = object});
  } else {
    install_locked(copy, (struct descriptor_entry){0});
  }
  libc_lock_release(&table_guard);
  descriptor_drop(held);
  return error ? fail(error) : copy;
}

/* Caller owns the detached alias reference; storage survives logical close. */
static int release_alias(struct descriptor_object *object, bool final, bool abandon)
{
  int result = 0;
  if (final) {
    int first_error = 0;
    if (!abandon && descriptor_object_transaction_pending(object)) {
      int locked = libc_mutex_lock(&object->read_guard);
      if (locked) {
        first_error = locked;
      } else {
        if (commit_locked(object) < 0) {
          first_error = errno;
        }
        unlock_object(&object->read_guard);
      }
    }
    if (!abandon && !first_error && object->transaction) {
      first_error = __atomic_load_n(&object->transaction->failure, __ATOMIC_ACQUIRE);
    }
    handle_t handles[6] = {object->handle, object->value};
    if (object->transaction) {
      handles[2] = object->transaction->control;
      handles[3] = object->transaction->body;
      handles[4] = object->transaction->response;
      handles[5] = object->transaction->failed_result.error_body;
    }
    for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
      bool duplicate = false;
      for (size_t j = 0; j < i; ++j) {
        duplicate |= handles[i] == handles[j];
      }
      if (handles[i] && !duplicate && descriptor_release_handle(handles[i]) < 0 && !first_error) {
        first_error = errno;
      }
    }
    if (first_error) {
      result = fail(first_error);
    }
  }
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

static int remove_original(struct descriptor_entry entry)
{
  static const char *const names[ENVIRONMENT_STREAM_COUNT] = {"stdin", "stdout", "stderr"};
  struct environment_unset_entry removed[1] = {0};
  size_t count = 0;
  if (entry.environment_identity) {
    environment_name(removed[count].name, names[entry.original_stream]);
    removed[count++].identity = entry.environment_identity;
  }
  enum call_status status = count ? environment_unset_batch(startup_environment_handle(), removed, count) : CALL_OK;
  return status == CALL_OK ? 0 : fail(libc_call_errno(status));
}

static int close_entry(int descriptor, FILE *stream, struct descriptor_object *expected,
    uint64_t generation, bool abandon)
{
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  if (!object || (expected && object != expected) ||
      (generation != UINT64_MAX && entries[descriptor].generation != generation) ||
      (stream && entries[descriptor].stream != stream)) {
    libc_lock_release(&table_guard);
    return fail(EBADF);
  }
  if (entries[descriptor].stream) {
    __atomic_store_n(&entries[descriptor].stream->descriptor, -1, __ATOMIC_RELEASE);
  }
  struct descriptor_entry original = entries[descriptor];
  install_locked(descriptor, (struct descriptor_entry){0});
  bool final = --object->references == 0;
  if (final) {
    __atomic_store_n(&object->closed, true, __ATOMIC_RELEASE);
  }
  libc_lock_release(&table_guard);
  /* Native ordinary CLOSE releases authority without waiting on its cursor;
   * transaction finalization drains under the shared cursor guard. */
  int cleanup = remove_original(original);
  int error = errno;
  int released = release_alias(object, final, abandon);
  return cleanup < 0 ? fail(error) : released;
}

int descriptor_close(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (!object) {
    return -1;
  }
  libc_lock_acquire(&table_guard);
  bool matches = lookup(descriptor) == object;
  uint64_t generation = matches ? entries[descriptor].generation : 0;
  libc_lock_release(&table_guard);
  int closed = !matches ? fail(EBADF) : descriptor_object_transaction_pending(object) ?
      libc_stdio_close_descriptor(descriptor, object, generation) :
      close_entry(descriptor, NULL, object, generation, false);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return closed;
}

int descriptor_close_expected(int descriptor, struct descriptor_object *object, uint64_t generation)
{
  return close_entry(descriptor, NULL, object, generation, false);
}

int descriptor_close_stream(FILE *stream, struct descriptor_object *object)
{
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  return close_entry(descriptor, stream, object, UINT64_MAX, false);
}

int descriptor_close_stream_abandon(FILE *stream, struct descriptor_object *object)
{
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  return close_entry(descriptor, stream, object, UINT64_MAX, true);
}

int dup2(int source, int target)
{
  if (target < 0) {
    return fail(EBADF);
  }
  if (source == target) {
    struct descriptor_object *object = descriptor_hold(source);
    if (!object) {
      return -1;
    }
    descriptor_drop(object);
    return target;
  }
  struct descriptor_object *held = descriptor_hold(source);
  if (!held) {
    return -1;
  }
  if (grow_entries((size_t)target + 1) < 0) {
    int error = errno;
    descriptor_drop(held);
    return fail(error);
  }
  libc_lock_acquire(&table_guard);
  struct descriptor_object *initial = lookup(source);
  if (!initial || initial != held) {
    libc_lock_release(&table_guard);
    descriptor_drop(held);
    return fail(EBADF);
  }
  uint64_t source_generation = entries[source].generation;
  uint64_t target_generation = entries[target].generation;
  struct descriptor_object *target_held = entries[target].state == DESCRIPTOR_OPEN ?
      entries[target].object : NULL;
  if (target_held && !retain_storage(target_held)) {
    libc_lock_release(&table_guard);
    descriptor_drop(held);
    return fail(EMFILE);
  }
  FILE *stream = entries[target].stream;
  if (stream && !stream_retain_association(stream)) {
    libc_lock_release(&table_guard);
    descriptor_drop(target_held);
    descriptor_drop(held);
    return fail(EBUSY);
  }
  libc_lock_release(&table_guard);
  if (stream && !stream_try_lock(stream)) {
    stream_release(stream);
    descriptor_drop(target_held);
    descriptor_drop(held);
    return fail(EBUSY);
  }
  int stage_error = 0;
  if (descriptor_object_transaction_pending(target_held) &&
      libc_stdio_stage_close_descriptor(target, target_held) < 0) {
    stage_error = errno;
  }
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(source);
  int error = stage_error ? stage_error : !object || object != held || entries[source].generation != source_generation ?
      EBADF : entries[target].generation != target_generation ||
      entries[target].state == DESCRIPTOR_RESERVED || entries[target].stream != stream || (stream && stream->output_count) ? EBUSY :
      __atomic_load_n(&object->storage, __ATOMIC_RELAXED) == SIZE_MAX ? EMFILE : 0;
  struct descriptor_object *previous = NULL;
  struct descriptor_entry original = {0};
  bool final = false;
  if (!error) {
    ++object->references;
    __atomic_add_fetch(&object->storage, 1, __ATOMIC_RELAXED);
    if (entries[target].state == DESCRIPTOR_OPEN) {
      original = entries[target];
      previous = entries[target].object;
      final = --previous->references == 0;
      if (final) {
        __atomic_store_n(&previous->closed, true, __ATOMIC_RELEASE);
      }
    }
    install_locked(target, (struct descriptor_entry){
      .state = DESCRIPTOR_RESERVED, .stream = stream,
    });
    if (stream) {
      __atomic_store_n(&stream->descriptor, -1, __ATOMIC_RELEASE);
    }
  }
  libc_lock_release(&table_guard);
  if (!error && previous) {
    int cleanup = remove_original(original);
    int cleanup_error = errno;
    int released = release_alias(previous, final, false);
    if (cleanup < 0 || released < 0) {
      error = cleanup < 0 ? cleanup_error : errno;
    }
  }
  if (!error) {
    publish(target, (struct descriptor_entry){
      .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
    });
    if (stream) {
      stream->has_pushback = false;
      stream->has_wide_pushback = false;
      stream->eof = false;
    }
  } else if (previous) {
    publish(target, (struct descriptor_entry){0});
    libc_lock_acquire(&table_guard);
    bool last = --object->references == 0;
    if (last) {
      __atomic_store_n(&object->closed, true, __ATOMIC_RELEASE);
    }
    libc_lock_release(&table_guard);
    release_alias(object, last, false);
  }
  if (stream) {
    stream_unlock(stream);
    stream_release(stream);
  }
  descriptor_drop(target_held);
  descriptor_drop(held);
  return error ? fail(error) : target;
}

void descriptor_report_finalize_error(void)
{
  int saved = errno;
  static const char message[] = "libc: transaction finalization failed\n";
  size_t written = 0;
  descriptor_write(2, message, sizeof(message) - 1, &written);
  errno = saved;
}

int descriptor_finish(void)
{
  /* Joined workers and closed thread admission precede orderly finalizers.
   * Keep diagnostics available until all nonstandard transactions are closed. */
  int error = 0;
  for (size_t pass = 0; pass < 2; ++pass) {
    size_t begin = pass ? 0 : ENVIRONMENT_STREAM_COUNT;
    size_t end = pass ? ENVIRONMENT_STREAM_COUNT : capacity;
    for (size_t i = begin; i < end; ++i) {
      if (entries[i].state == DESCRIPTOR_OPEN && descriptor_close((int)i) < 0 && !error) {
        error = errno;
        descriptor_report_finalize_error();
      }
    }
  }
  if (entries != startup_entries) {
    free(entries);
  }
  entries = startup_entries;
  capacity = ENVIRONMENT_STREAM_COUNT;
  memset(startup_entries, 0, sizeof(startup_entries));
  return error ? fail(error) : 0;
}

/* A value-capable pipe stops at each value. Receive it and read it through to
 * its end as ordinary file bytes, then continue with the pipe, so byte readers
 * see one stream. A failed value keeps failing its reads with its outcome.
 * The try form waits for nothing on the pipe; reading a value can still wait
 * for its file's own I/O. Returns zero or an errno. */
static int pipe_segment_read(struct descriptor_object *entry, void *buffer,
    size_t size, size_t *read, bool try)
{
  for (;;) {
    enum call_status status;
    if (entry->value) {
      status = file_read(entry->value, entry->value_offset, buffer, size, read);
      if (status == CALL_ENDPOINT_CLOSED || status == CALL_TIMED_OUT) {
        /* A provider that exited or stalled is an I/O error, never EPIPE or
         * a retryable EAGAIN. */
        return EIO;
      }
      if (status != CALL_OK) {
        return libc_call_errno(status);
      }
      if (*read) {
        entry->value_offset += *read;
        return 0;
      }
      handle_t finished = entry->value;
      entry->value = HANDLE_INVALID;
      entry->value_offset = 0;
      if (descriptor_release_handle(finished) < 0) {
        return EIO;
      }
    }
    status = try ? pipe_try_read(entry->handle, buffer, size, read) :
        pipe_read(entry->handle, buffer, size, read);
    if (status != CALL_PIPE_VALUE) {
      return status == CALL_OK ? 0 : libc_call_errno(status);
    }
    uint64_t head;
    handle_t value;
    status = try ? pipe_try_receive(entry->handle, &head, &value) :
        pipe_receive(entry->handle, &head, &value);
    if (status == CALL_LIMIT) {
      /* No free handle slot for the value. */
      return EMFILE;
    }
    if (status != CALL_OK) {
      return libc_call_errno(status);
    }
    /* BYTES or END means a copied reader took the value first; read again. */
    if (head == PIPE_HEAD_VALUE) {
      entry->value = value;
    }
  }
}

/* One backend transfer following any read-ahead; does not move the position. */
static int backend_read(struct descriptor_object *entry, void *buffer, size_t size,
    size_t *read)
{
  enum call_status status;
  if (entry->transaction && !entry->transaction->committed && entry->readable) {
    return fail(EBUSY);
  }
  if (entry->kind == DESCRIPTOR_RESPONSE) {
    if (entry->final_checked) {
      *read = 0;
      return entry->final_error ? fail(entry->final_error) : 0;
    }
    size_t capacity = size < STREAM_READ_MAX_BYTES ? size : STREAM_READ_MAX_BYTES;
    status = stream_read(entry->handle, buffer, capacity, 0, read);
    if (status != CALL_OK) {
      entry->final_checked = true;
      entry->final_error = status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ?
          EIO : libc_call_errno(status);
      return fail(entry->final_error);
    }
    if (status == CALL_OK && !*read) {
      struct describe_reply final;
      status = handle_describe(entry->handle, 0, &final);
      entry->final_checked = true;
      int error = status == CALL_OK ? resource_errno(final.description.outcome) :
          status == CALL_DENIED ? EACCES : EIO;
      if (error || !(final.description.flags & RESOURCE_COMPLETE)) {
        entry->final_error = error ? error : EIO;
        return fail(entry->final_error);
      }
    }
  } else if (entry->kind == DESCRIPTOR_FILE) {
    uint64_t offset = entry->position + entry->ahead_count;
    if (size > UINT64_MAX - offset) {
      return fail(EOVERFLOW);
    }
    status = file_read(entry->handle, offset, buffer, size, read);
  } else if (entry->kind == DESCRIPTOR_PIPE) {
    int error = pipe_segment_read(entry, buffer, size, read, false);
    if (error) {
      *read = 0;
      return fail(error);
    }
    status = CALL_OK;
  } else {
    status = console_read(entry->handle, buffer, size, read);
  }
  if (status != CALL_OK) {
    *read = 0;
    return fail(libc_call_errno(status));
  }
  if (*read) {
    entry->input_consumed = true;
  }
  if (*read > size) {
    *read = 0;
    return fail(EIO);
  }
  return 0;
}

static size_t take_ahead(struct descriptor_object *entry, void *buffer, size_t size)
{
  size_t count = size < entry->ahead_count ? size : entry->ahead_count;
  memcpy(buffer, entry->ahead + entry->ahead_start, count);
  entry->ahead_start += count;
  entry->ahead_count -= count;
  if (!entry->ahead_count) {
    entry->ahead_start = 0;
  }
  if (entry->kind == DESCRIPTOR_FILE) {
    entry->position += count;
  }
  return count;
}

static struct descriptor_object *try_entry(struct descriptor_object *entry, size_t count, bool writing)
{
  if (object_ready(entry, writing) < 0) {
    return NULL;
  }
  if (count > (size_t)SSIZE_MAX) {
    errno = EINVAL;
    return NULL;
  }
  if (entry->kind == DESCRIPTOR_FILE || entry->kind == DESCRIPTOR_RESPONSE) {
    errno = ENOTSUP;
    return NULL;
  }
  return entry;
}

static ssize_t try_read_locked(struct descriptor_object *entry, void *buffer, size_t count)
{
  entry = try_entry(entry, count, false);
  if (!entry) {
    return -1;
  }
  if (!count) {
    return 0;
  }
  if (entry->ahead_count) {
    return (ssize_t)take_ahead(entry, buffer, count);
  }
  size_t read;
  if (entry->kind == DESCRIPTOR_PIPE) {
    int error = pipe_segment_read(entry, buffer, count, &read, true);
    if (error) {
      return fail(error);
    }
  } else {
    enum call_status status = console_read_timeout(entry->handle, buffer, count, 0, &read);
    if (status != CALL_OK) {
      return fail(status == CALL_TIMED_OUT ? EAGAIN : libc_call_errno(status));
    }
  }
  if (read > count) {
    return fail(EIO);
  }
  return (ssize_t)read;
}

static ssize_t try_write_locked(struct descriptor_object *entry, const void *buffer, size_t count)
{
  entry = try_entry(entry, count, true);
  if (!entry) {
    return -1;
  }
  if (!count) {
    return 0;
  }
  size_t written;
  enum call_status status = entry->kind == DESCRIPTOR_PIPE ?
      pipe_try_write(entry->handle, buffer, count, &written) :
      console_try_write(entry->handle, buffer, count, &written);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  if (!written || written > count) {
    return fail(EIO);
  }
  return (ssize_t)written;
}

static void discard_ahead(struct descriptor_object *entry)
{
  entry->ahead_start = 0;
  entry->ahead_count = 0;
}

static bool ahead_available(struct descriptor_object *entry)
{
  if (entry->ahead) {
    return true;
  }
  if (entry->unbuffered) {
    return false;
  }
  /* Read-ahead is only an optimization: failure is neither an error nor
   * repeated per read, and must not disturb the caller's errno. */
  int saved = errno;
  entry->ahead = malloc(BUFSIZ);
  errno = saved;
  if (!entry->ahead) {
    entry->unbuffered = true;
    return false;
  }
  return true;
}

static int read_exact(struct descriptor_object *entry, void *buffer, size_t size,
    size_t *read)
{
  if (backend_read(entry, buffer, size, read) < 0) {
    return -1;
  }
  if (entry->kind == DESCRIPTOR_FILE) {
    entry->position += *read;
  }
  return 0;
}

static int read_locked(struct descriptor_object *entry, void *buffer, size_t size, size_t *read)
{
  *read = 0;
  if (object_ready(entry, false) < 0) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  if (entry->ahead_count) {
    *read = take_ahead(entry, buffer, size);
    return 0;
  }
  return read_exact(entry, buffer, size, read);
}

static int read_complete_locked(struct descriptor_object *entry, void *buffer, size_t size, size_t *read)
{
  if (read_locked(entry, buffer, size, read) < 0) {
    return -1;
  }
  while (*read < size && entry->kind == DESCRIPTOR_FILE) {
    size_t more;
    if (read_locked(entry, (char *)buffer + *read, size - *read, &more) < 0) {
      return 0;
    }
    if (!more) {
      break;
    }
    *read += more;
  }
  return 0;
}

static int read_buffered_locked(struct descriptor_object *entry, void *buffer, size_t size, size_t *read)
{
  *read = 0;
  if (object_ready(entry, false) < 0) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  if (entry->ahead_count) {
    *read = take_ahead(entry, buffer, size);
    return 0;
  }
  /* Consoles are shared interactive input and are never read ahead. A request
   * of at least one buffer gains nothing from copying through it. */
  if (entry->kind == DESCRIPTOR_CONSOLE || size >= BUFSIZ || !ahead_available(entry)) {
    return read_exact(entry, buffer, size, read);
  }
  /* Speculation must not turn a valid read into an error. A file fill stays
   * below LONG_MAX, the stdio position range; host:// also rejects reads that
   * end past the signed offset range. At or past that boundary, or if even the
   * request does not fit below it, the exact read keeps the backend's result. */
  size_t fill = BUFSIZ;
  if (entry->kind == DESCRIPTOR_FILE) {
    uint64_t limit = LONG_MAX;
    uint64_t remaining = entry->position < limit ? limit - entry->position : 0;
    if (remaining < fill) {
      fill = (size_t)remaining;
    }
  }
  if (fill < size) {
    return read_exact(entry, buffer, size, read);
  }
  size_t fetched;
  if (backend_read(entry, entry->ahead, fill, &fetched) < 0) {
    return -1;
  }
  entry->ahead_start = 0;
  entry->ahead_count = fetched;
  *read = take_ahead(entry, buffer, size);
  return 0;
}

static void discard_input_locked(struct descriptor_object *entry)
{
  if (entry->kind == DESCRIPTOR_FILE) {
    discard_ahead(entry);
  }
}

static int write_locked(struct descriptor_object *entry, const void *buffer, size_t size, size_t *written)
{
  *written = 0;
  if (object_ready(entry, true) < 0) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  /* Files can refetch speculative bytes at the shared logical position.
   * Pipe bytes already consumed from the backend must remain available. */
  if (entry->kind == DESCRIPTOR_FILE) {
    discard_ahead(entry);
  }
  uint64_t position = entry->position;
  enum call_status status;
  if (entry->kind == DESCRIPTOR_FILE) {
    if (__atomic_load_n(&entry->append, __ATOMIC_ACQUIRE)) {
      /* SIZE followed by WRITE preserves stdio's non-atomic append behavior. */
      status = file_size(entry->handle, &position);
      if (status != CALL_OK) {
        return fail(libc_call_errno(status));
      }
    }
    if (size > UINT64_MAX - position) {
      return fail(EOVERFLOW);
    }
    status = file_write(entry->handle, position, buffer, size, written);
  } else if (entry->kind == DESCRIPTOR_PIPE) {
    status = pipe_write(entry->handle, buffer, size, written);
  } else {
    status = console_write(entry->handle, buffer, size, written);
  }
  if (status != CALL_OK) {
    if (entry->transaction) {
      entry->transaction->producer_failed = true;
    }
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    if (entry->transaction) {
      entry->transaction->producer_failed = true;
    }
    *written = 0;
    return fail(EIO);
  }
  if (entry->kind == DESCRIPTOR_FILE) {
    entry->position = position + *written;
  }
  return 0;
}

static int pread_locked(struct descriptor_object *entry, void *buffer, size_t size, uint64_t offset,
    size_t *read)
{
  *read = 0;
  if (object_ready(entry, false) < 0) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(ESPIPE);
  }
  if (!size) {
    return 0;
  }
  if (size > UINT64_MAX - offset) {
    return fail(EOVERFLOW);
  }
  handle_t handle = entry->handle;
  enum call_status status = file_read(handle, offset, buffer, size, read);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  if (*read) {
    entry->input_consumed = true;
  }
  if (*read > size) {
    *read = 0;
    return fail(EIO);
  }
  return 0;
}

static int pwrite_locked(struct descriptor_object *entry, const void *buffer, size_t size, uint64_t offset,
    size_t *written)
{
  *written = 0;
  if (object_ready(entry, true) < 0) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(ESPIPE);
  }
  if (!size) {
    return 0;
  }
  if (size > UINT64_MAX - offset) {
    return fail(EOVERFLOW);
  }
  /* A failed native mutation can still have changed the file. Drop speculative
   * bytes before dispatch, leaving the logical position available for refetch. */
  discard_ahead(entry);
  handle_t handle = entry->handle;
  enum call_status status = file_write(handle, offset, buffer, size, written);
  if (status != CALL_OK) {
    if (entry->transaction) {
      entry->transaction->producer_failed = true;
    }
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    if (entry->transaction) {
      entry->transaction->producer_failed = true;
    }
    *written = 0;
    return fail(EIO);
  }
  return 0;
}

static int seek_locked(struct descriptor_object *entry, long offset, int origin)
{
  if (!entry) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(ESPIPE);
  }
  uint64_t base;
  if (origin == SEEK_SET) {
    base = 0;
  } else if (origin == SEEK_CUR) {
    base = entry->position;
  } else if (origin == SEEK_END) {
    enum call_status status = file_size(entry->handle, &base);
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
  } else {
    return fail(EINVAL);
  }
  uint64_t distance = offset < 0 ? 0 - (uint64_t)offset : (uint64_t)offset;
  if ((offset < 0 && distance > base) || (offset >= 0 && distance > UINT64_MAX - base)) {
    return fail(offset < 0 ? EINVAL : EOVERFLOW);
  }
  /* Only a validated seek drops read-ahead; failures keep every byte. */
  discard_ahead(entry);
  entry->position = offset < 0 ? base - distance : base + distance;
  return 0;
}

static int resize_locked(struct descriptor_object *entry, uint64_t size)
{
  if (!entry) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(EINVAL);
  }
  /* Read-ahead may hold bytes the resize removes or zero-fills. Drop it even
   * if the outcome is uncertain; files can fetch the current bytes again. */
  discard_ahead(entry);
  enum call_status status = file_resize(entry->handle, size);
  if (status != CALL_OK) {
    if (entry->transaction) {
      entry->transaction->producer_failed = true;
    }
    return fail(libc_call_errno(status));
  }
  return 0;
}

static int sync_locked(struct descriptor_object *entry)
{
  if (!entry) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(EINVAL);
  }
  enum call_status status = file_sync(entry->handle);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  return 0;
}

static int stat_locked(struct descriptor_object *entry, struct stat *result)
{
  if (!entry) {
    return -1;
  }
  if (entry->kind == DESCRIPTOR_CONSOLE) {
    *result = (struct stat){.st_mode = S_IFCHR};
    return 0;
  }
  if (entry->kind == DESCRIPTOR_RESPONSE) {
    return fail(ENOTSUP);
  }
  if (entry->kind == DESCRIPTOR_PIPE) {
    *result = (struct stat){.st_mode = S_IFIFO};
    return 0;
  }
  enum call_status status = libc_file_stat(entry->handle, result);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  return 0;
}

static int terminal_locked(struct descriptor_object *entry)
{
  if (!entry) {
    return 0;
  }
  if (entry->kind != DESCRIPTOR_CONSOLE) {
    errno = ENOTTY;
    return 0;
  }
  return 1;
}

static long tell_output_locked(struct descriptor_object *entry, size_t pending)
{
  if (!entry) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(ESPIPE);
  }
  uint64_t position = entry->position;
  if (pending && __atomic_load_n(&entry->append, __ATOMIC_ACQUIRE)) {
    handle_t handle = entry->handle;
    enum call_status status = file_size(handle, &position);
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
  }
  if (position > LONG_MAX || pending > (uint64_t)LONG_MAX - position) {
    return fail(EOVERFLOW);
  }
  return (long)(position + pending);
}

static long tell_locked(struct descriptor_object *entry)
{
  return tell_output_locked(entry, 0);
}

/* Operation locks serialize mutable state, never native grant ownership. */
static struct libc_mutex *object_guard(struct descriptor_object *object, bool writing)
{
  return object->transaction ? &object->read_guard :
      writing && object->kind != DESCRIPTOR_FILE ? &object->write_guard : &object->read_guard;
}

static int object_enter(struct descriptor_object *object, bool writing)
{
  if (!object || __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    return fail(EBADF);
  }
  struct libc_mutex *guard = object_guard(object, writing);
  int error = libc_mutex_lock(guard);
  if (error) {
    return fail(error);
  }
  if (__atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    unlock_object(guard);
    return fail(EBADF);
  }
  return 0;
}

bool descriptor_object_ready(struct descriptor_object *object, bool writing)
{
  return object_ready(object, writing) == 0;
}

int descriptor_object_stream(struct descriptor_object *object, struct pyxis_stdio_binding *binding)
{
  if (!object || __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    return fail(EBADF);
  }
  if (object->transaction && !object->transaction->committed && object->kind == DESCRIPTOR_RESPONSE) {
    *binding = (struct pyxis_stdio_binding){
      .protocol = PROTOCOL_TRANSACTION,
      .handle = object->transaction->control,
    };
    return 0;
  }
  *binding = (struct pyxis_stdio_binding){
    .protocol = object->kind == DESCRIPTOR_FILE ? PROTOCOL_FILE :
        object->kind == DESCRIPTOR_PIPE ? PROTOCOL_PIPE :
          object->kind == DESCRIPTOR_RESPONSE ? PROTOCOL_STREAM : PROTOCOL_CONSOLE,
    .handle = object->handle,
    .append = __atomic_load_n(&object->append, __ATOMIC_ACQUIRE),
  };
  return 0;
}

enum call_status descriptor_object_output_copy(struct descriptor_object *object,
    struct output_handoff_request *owned)
{
  *owned = (struct output_handoff_request){0};
  if (object_enter(object, false) < 0) {
    return CALL_BAD_HANDLE;
  }
  enum call_status status = CALL_BAD_OPERATION;
  if ((object->kind == DESCRIPTOR_FILE || object->kind == DESCRIPTOR_RESPONSE) &&
      object->readable && !object->writable && !object->position &&
      !object->ahead_count && !object->input_consumed &&
      (!object->transaction || object->transaction->committed)) {
    struct describe_reply description;
    status = handle_describe(object->handle, 0, &description);
    uint64_t read_right = object->kind == DESCRIPTOR_FILE ? FILE_RIGHT_READ : STREAM_RIGHT_READ;
    if (status == CALL_OK && description.description.outcome == RESOURCE_OK &&
        !(description.description.flags & RESOURCE_REREADABLE)) {
      status = CALL_BAD_OPERATION;
    }
    if (status == CALL_OK) {
      status = handle_copy_restricted(object->handle, description.rights & read_right,
          description.transport, &owned->value);
    }
  }
  if (status != CALL_OK) {
    unlock_object(object_guard(object, false));
    if (owned->value) {
      descriptor_release_handle(owned->value);
      *owned = (struct output_handoff_request){0};
    }
  }
  return status;
}

int descriptor_object_read(struct descriptor_object *object, void *buffer, size_t size, size_t *read)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = read_locked(object, buffer, size, read);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_read(int descriptor, void *buffer, size_t size, size_t *read)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_read(object, buffer, size, read);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_read_complete(struct descriptor_object *object, void *buffer, size_t size, size_t *read)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = read_complete_locked(object, buffer, size, read);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_read_complete(int descriptor, void *buffer, size_t size, size_t *read)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_read_complete(object, buffer, size, read);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_read_buffered(struct descriptor_object *object, void *buffer, size_t size, size_t *read)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = read_buffered_locked(object, buffer, size, read);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_read_buffered(int descriptor, void *buffer, size_t size, size_t *read)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_read_buffered(object, buffer, size, read);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_write(struct descriptor_object *object, const void *buffer, size_t size, size_t *written)
{
  if (object_enter(object, true) < 0) {
    return -1;
  }
  int result = write_locked(object, buffer, size, written);
  unlock_object(object_guard(object, true));
  return result;
}

int descriptor_write(int descriptor, const void *buffer, size_t size, size_t *written)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_write(object, buffer, size, written);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_pread(struct descriptor_object *object, void *buffer, size_t size, uint64_t offset, size_t *read)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = pread_locked(object, buffer, size, offset, read);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_pread(int descriptor, void *buffer, size_t size, uint64_t offset, size_t *read)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_pread(object, buffer, size, offset, read);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_pwrite(struct descriptor_object *object, const void *buffer, size_t size, uint64_t offset, size_t *written)
{
  if (object_enter(object, true) < 0) {
    return -1;
  }
  int result = pwrite_locked(object, buffer, size, offset, written);
  unlock_object(object_guard(object, true));
  return result;
}

int descriptor_pwrite(int descriptor, const void *buffer, size_t size, uint64_t offset, size_t *written)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_pwrite(object, buffer, size, offset, written);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_seek(struct descriptor_object *object, long offset, int origin)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = seek_locked(object, offset, origin);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_seek(int descriptor, long offset, int origin)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_seek(object, offset, origin);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_resize(struct descriptor_object *object, uint64_t size)
{
  if (object_enter(object, true) < 0) {
    return -1;
  }
  int result = resize_locked(object, size);
  unlock_object(object_guard(object, true));
  return result;
}

int descriptor_resize(int descriptor, uint64_t size)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_resize(object, size);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_sync(struct descriptor_object *object)
{
  if (object_enter(object, true) < 0) {
    return -1;
  }
  int result = sync_locked(object);
  unlock_object(object_guard(object, true));
  return result;
}

int descriptor_sync(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_sync(object);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_stat(struct descriptor_object *object, struct stat *metadata)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = stat_locked(object, metadata);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_stat(int descriptor, struct stat *metadata)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_stat(object, metadata);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

int descriptor_object_terminal(struct descriptor_object *object)
{
  if (object_enter(object, false) < 0) {
    return 0;
  }
  int result = terminal_locked(object);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_terminal(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_terminal(object);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

long descriptor_object_tell_output(struct descriptor_object *object, size_t pending)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  long result = tell_output_locked(object, pending);
  unlock_object(object_guard(object, false));
  return result;
}

long descriptor_tell_output(int descriptor, size_t pending)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  long result = descriptor_object_tell_output(object, pending);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

long descriptor_object_tell(struct descriptor_object *object)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  long result = tell_locked(object);
  unlock_object(object_guard(object, false));
  return result;
}

long descriptor_tell(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  long result = descriptor_object_tell(object);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

void descriptor_object_discard_input(struct descriptor_object *object)
{
  if (object_enter(object, false) < 0) {
    return;
  }
  discard_input_locked(object);
  unlock_object(object_guard(object, false));
}

void descriptor_discard_input(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  descriptor_object_discard_input(object);
  int error = errno;
  descriptor_drop(object);
  errno = error;
}

ssize_t pyxis_descriptor_try_read(int descriptor, void *buffer, size_t count)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (object_enter(object, false) < 0) {
    descriptor_drop(object);
    return -1;
  }
  ssize_t result = try_read_locked(object, buffer, count);
  unlock_object(object_guard(object, false));
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

ssize_t pyxis_descriptor_try_write(int descriptor, const void *buffer, size_t count)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (object_enter(object, true) < 0) {
    descriptor_drop(object);
    return -1;
  }
  ssize_t result = try_write_locked(object, buffer, count);
  unlock_object(object_guard(object, true));
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

long descriptor_seek_position(int descriptor, long offset, int origin)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (object_enter(object, false) < 0) {
    descriptor_drop(object);
    return -1;
  }
  long result = seek_locked(object, offset, origin) < 0 ? -1 : tell_locked(object);
  unlock_object(&object->read_guard);
  int error = errno;
  descriptor_drop(object);
  errno = error;
  return result;
}

/* An outer FILE operation serializes complete regular-file transfers. Pipe
 * and console directions remain independent and are locked by each transfer. */
bool descriptor_file_begin(struct descriptor_object *object)
{
  if (!object || (!object->transaction && object->kind != DESCRIPTOR_FILE) ||
      __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    return false;
  }
  return object_enter(object, false) == 0;
}

int descriptor_object_take_value(struct descriptor_object *object, handle_t *value)
{
  *value = HANDLE_INVALID;
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = 0;
  if (object->kind == DESCRIPTOR_PIPE && !object->ahead_count) {
    if (object->value) {
      /* A value counts only before any of its bytes were read. */
      if (!object->value_offset) {
        *value = object->value;
        object->value = HANDLE_INVALID;
        result = 1;
      }
    } else {
      uint64_t head;
      enum call_status status = pipe_receive(object->handle, &head, value);
      if (status != CALL_OK) {
        result = fail(status == CALL_LIMIT ? EMFILE : libc_call_errno(status));
      } else if (head == PIPE_HEAD_VALUE) {
        object->input_consumed = true;
        result = 1;
      }
    }
  }
  unlock_object(object_guard(object, false));
  return result;
}

void descriptor_file_end(struct descriptor_object *object)
{
  unlock_object(&object->read_guard);
}

bool descriptor_object_transaction(struct descriptor_object *object)
{
  return object && object->transaction;
}

bool descriptor_object_transaction_pending(struct descriptor_object *object)
{
  return descriptor_object_transaction(object) &&
      !__atomic_load_n(&object->transaction->committed, __ATOMIC_ACQUIRE);
}

static int drain_reply(struct descriptor_transaction *transaction, const struct resource_result *result)
{
  if (result->info.protocol == PROTOCOL_STREAM) {
    unsigned char buffer[STREAM_READ_MAX_BYTES];
    for (;;) {
      size_t count = 0;
      enum call_status status = stream_read(result->resource, buffer, sizeof(buffer),
          transaction->deadline, &count);
      if (status != CALL_OK) {
        struct describe_reply final;
        if (handle_describe(result->resource, DESCRIBE_INCLUDE_ERROR_BODY, &final) == CALL_OK) {
          if (final.description.outcome != RESOURCE_OK) {
            if (transaction->failed_result.error_body) {
              handle_close(transaction->failed_result.error_body);
            }
            transaction->result_failed = true;
            transaction->failed_result = final;
          } else if (final.error_body) {
            handle_close(final.error_body);
          }
        }
        return fail(status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ?
            EIO : libc_call_errno(status));
      }
      if (!count) {
        struct describe_reply final;
        status = handle_describe(result->resource, DESCRIBE_INCLUDE_ERROR_BODY, &final);
        if (status != CALL_OK) {
          return fail(status == CALL_DENIED ? EACCES : EIO);
        }
        int error = resource_errno(final.description.outcome);
        if (error || !(final.description.flags & RESOURCE_COMPLETE)) {
          if (transaction->failed_result.error_body) {
            handle_close(transaction->failed_result.error_body);
          }
          transaction->result_failed = true;
          transaction->failed_result = final;
          return fail(error ? error : EIO);
        }
        if (final.error_body) {
          handle_close(final.error_body);
        }
        break;
      }
    }
  }
  return 0;
}

/* The shared cursor guard is held. Logical final CLOSE has already invalidated
 * aliases; it still owns storage and drains admitted staging before submission. */
static int commit_locked(struct descriptor_object *object)
{
  struct descriptor_transaction *transaction = object->transaction;
  if (!transaction || transaction->committed) {
    return 0;
  }
  if (transaction->attempted || transaction->producer_failed) {
    return fail(transaction->failure ? transaction->failure : EIO);
  }
  transaction->attempted = true;
  libc_lock_acquire(&table_guard);
  __atomic_store_n(&object->writable, false, __ATOMIC_RELEASE);
  libc_lock_release(&table_guard);
  struct resource_result result = {0};
  enum call_status status = transaction_commit(transaction->control,
      transaction->deadline, &result);
  int error = status == CALL_OK ? resource_errno(result.info.description.outcome) :
      status == CALL_TIMED_OUT || status == CALL_ENDPOINT_CLOSED ? EIO : libc_call_errno(status);
  if (status == CALL_OK) {
    /* A returned outcome is definitive; later flushes do not submit again even
     * when the outcome reports failure. Reader operations retain that failure. */
    libc_lock_acquire(&table_guard);
    __atomic_store_n(&transaction->committed, true, __ATOMIC_RELEASE);
    if (error || result.info.error_body) {
      transaction->result_failed = true;
      transaction->failed_result = result.info;
      result.info.error_body = HANDLE_INVALID;
    }
    if (result.resource && transaction->inplace) {
      transaction->response = result.resource;
      object->handle = result.resource;
      object->kind = result.info.protocol == PROTOCOL_FILE ? DESCRIPTOR_FILE : DESCRIPTOR_RESPONSE;
      discard_ahead(object);
      object->position = 0;
      result.resource = HANDLE_INVALID;
    }
    libc_lock_release(&table_guard);
    if (!transaction->inplace && result.resource && !error && drain_reply(transaction, &result) < 0) {
      error = errno;
    }
  }
  uint64_t outcome = result.info.description.outcome;
  resource_result_close(&result);
  if (error) {
    __atomic_store_n(&transaction->failure, error, __ATOMIC_RELEASE);
    transaction->failure_outcome = status == CALL_OUTCOME_UNKNOWN ? RESOURCE_UNCERTAIN :
        status == CALL_ABANDONED ? RESOURCE_ABORTED :
        status == CALL_DENIED ? RESOURCE_DENIED :
        status == CALL_NOT_FOUND ? RESOURCE_NOT_FOUND :
        status == CALL_FILE_TOO_LARGE ? RESOURCE_TOO_LARGE :
        status == CALL_OK && outcome != RESOURCE_OK ? outcome : RESOURCE_IO_ERROR;
  }
  return error ? fail(error) : 0;
}

int descriptor_object_commit(struct descriptor_object *object)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = commit_locked(object);
  int error = errno;
  unlock_object(&object->read_guard);
  errno = error;
  return result;
}

int descriptor_object_abandon(struct descriptor_object *object, int failure)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  struct descriptor_transaction *transaction = object->transaction;
  enum call_status status = CALL_BAD_OPERATION;
  if (transaction) {
    transaction->producer_failed = true;
    /* Cleanup cannot replace an earlier failure or make an uncertain COMMIT
     * outcome appear canceled: ABORT cannot undo possible remote effects. */
    if (!transaction->failure) {
      __atomic_store_n(&transaction->failure, failure ? failure : ECANCELED, __ATOMIC_RELEASE);
      transaction->failure_outcome = failure == ECANCELED ? RESOURCE_ABORTED : RESOURCE_IO_ERROR;
    }
    libc_lock_acquire(&table_guard);
    __atomic_store_n(&object->writable, false, __ATOMIC_RELEASE);
    libc_lock_release(&table_guard);
    if (!transaction->committed) {
      status = transaction_abort(transaction->control, transaction->deadline);
    } else {
      status = CALL_OK;
    }
  }
  unlock_object(&object->read_guard);
  return status == CALL_OK ? 0 : fail(libc_call_errno(status));
}

int descriptor_object_describe(struct descriptor_object *object, uint64_t flags,
    struct describe_reply *reply)
{
  if (!reply) {
    return fail(EINVAL);
  }
  *reply = (struct describe_reply){0};
  if (object_enter(object, false) < 0) {
    return -1;
  }
  struct descriptor_transaction *transaction = object->transaction;
  handle_t target = transaction && (!transaction->committed || !transaction->inplace) ?
      transaction->control : object->handle;
  enum call_status status;
  bool captured = transaction && transaction->result_failed;
  status = handle_describe(target, captured ? 0 : flags, reply);
  if (status == CALL_OK && captured) {
    /* Kernel type, masks and cut state are queried again. A definitive failed
     * result retains its declared detail/body, not authority over its issuer. */
    if (reply->description.outcome != RESOURCE_DENIED) {
      reply->description = transaction->failed_result.description;
    }
    if (transaction->failed_result.error_body) {
      reply->description.flags |= RESOURCE_ERROR_BODY;
      if (flags & DESCRIBE_INCLUDE_ERROR_BODY) {
        status = handle_copy_restricted(transaction->failed_result.error_body,
            FILE_RIGHT_READ, 0, &reply->error_body);
      }
    }
  }
  if (status == CALL_OK && transaction && transaction->failure &&
      (reply->description.outcome == RESOURCE_OK ||
          (transaction->failure_outcome == RESOURCE_UNCERTAIN &&
              reply->description.outcome != RESOURCE_DENIED))) {
    /* Local producer failure or uncertain delivery cannot be repaired by a
     * stale issuer snapshot. No length/completion is fabricated. */
    uint64_t body = reply->description.flags & RESOURCE_ERROR_BODY;
    reply->description = (struct resource_description){
      .outcome = transaction->failure_outcome ? transaction->failure_outcome : RESOURCE_IO_ERROR,
      .flags = body,
    };
  }
  unlock_object(&object->read_guard);
  return status == CALL_OK ? 0 : fail(libc_call_errno(status));
}
