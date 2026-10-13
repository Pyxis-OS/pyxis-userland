#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <console.h>
#include <clock.h>
#include <http_transaction.h>
#include <provider.h>
#include <pyxis/working_path.h>
#include <stream.h>
#include <directory.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <pipe.h>
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

enum descriptor_state { DESCRIPTOR_FREE, DESCRIPTOR_RESERVED, DESCRIPTOR_OPEN };
enum descriptor_kind { DESCRIPTOR_CONSOLE, DESCRIPTOR_FILE, DESCRIPTOR_PIPE, DESCRIPTOR_RESPONSE };

/* Each descriptor owns one reference to an open object. Duplication shares its
 * native handle, access, append policy, logical position and unread read-ahead.
 * Position is the next byte delivered; backend reads start after read-ahead. */
struct descriptor_http {
  struct http_prepared prepared;
  uint64_t deadline;
  bool inplace, attempted, committed, producer_failed;
  int failure;
};

struct descriptor_object {
  size_t references; /* Descriptor aliases, protected by table_guard. */
  size_t storage; /* Aliases and admitted operations; no native grant copies. */
  bool closed;
  struct libc_mutex read_guard, write_guard;
  bool allocated;
  enum descriptor_kind kind;
  handle_t handle;
  handle_t observation;
  struct descriptor_http *http;
  bool final_checked;
  int final_error;
  uint64_t position;
  bool readable, writable, append;
  bool unbuffered; /* Read-ahead allocation failed; stay exact until close. */
  unsigned char *ahead; /* Owned, BUFSIZ bytes, allocated on first fill. */
  size_t ahead_start, ahead_count;
  struct pyxis_response_info *response; /* Owned metadata for this open. */
};

struct descriptor_entry {
  uint64_t generation;
  enum descriptor_state state;
  struct descriptor_object *object;
  FILE *stream; /* Non-owning; only one FILE can associate with this slot. */
};

static struct descriptor_object startup_objects[STARTUP_STREAM_COUNT];
static struct descriptor_entry startup_entries[STARTUP_STREAM_COUNT];
static struct descriptor_entry *entries = startup_entries;
static size_t capacity = STARTUP_STREAM_COUNT;
static struct libc_lock table_guard;

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

struct descriptor_object *descriptor_hold(int descriptor)
{
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  if (object) {
    size_t count = __atomic_load_n(&object->storage, __ATOMIC_RELAXED);
    while (count != SIZE_MAX && !__atomic_compare_exchange_n(&object->storage,
        &count, count + 1, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (count == SIZE_MAX) {
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
  free(object->response);
  free(object->http);
  if (object->allocated) {
    free(object);
  }
}

static int object_ready(struct descriptor_object *object, bool writing)
{
  if (!object || __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE) ||
      (writing ? !object->writable : !object->readable)) {
    return fail(EBADF);
  }
  if (!writing && object->http && !object->http->committed) {
    return fail(object->http->failure ? object->http->failure : EBUSY);
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

void descriptor_adopt_standard(enum startup_stream_index index, FILE *stream)
{
  struct startup_stream binding = startup_stream(index);
  if (binding.protocol == STARTUP_STREAM_NONE) {
    return;
  }
  /* Startup already validated distinct handles, protocols and stream rights.
   * Adopt without allocation or a copy that would prolong endpoint lifetime. */
  struct descriptor_object *object = &startup_objects[index];
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1,
    .kind = binding.protocol == PROTOCOL_FILE ? DESCRIPTOR_FILE :
            binding.protocol == PROTOCOL_PIPE ? DESCRIPTOR_PIPE :
            binding.protocol == PROTOCOL_STREAM ? DESCRIPTOR_RESPONSE : DESCRIPTOR_CONSOLE,
    .handle = binding.handle, .observation = binding.observation,
    .readable = index == STARTUP_STDIN,
    .writable = index != STARTUP_STDIN,
  };
  entries[index] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  };
  stream->descriptor = (int)index;
}

int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream)
{
  if (provider_http_uri(path) || (path && !strncmp(path, "json+", 5) && provider_http_uri(path + 5))) {
    return descriptor_http_open(path, mode, stream, NULL, !mode->writable);
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
  handle_t handle = HANDLE_INVALID;
  uint64_t position = 0;
  uint64_t rights = (mode->readable ? FILE_RIGHT_READ : 0) |
                    (mode->writable ? FILE_RIGHT_WRITE : 0);
  struct pyxis_response_info info = {0};
  struct pyxis_response_info *response = NULL;
  enum call_status status = mode->exclusive ? file_create_path(path, rights, &handle) :
      file_open_path_response(path, rights, mode->create, &info, &handle);
  if (status == CALL_OK && info.flags) {
    response = malloc(sizeof(*response));
    if (!response) {
      status = CALL_NO_MEMORY;
    } else {
      *response = info;
    }
  }
  if (status == CALL_OK && mode->append && !mode->readable) {
    status = file_size(handle, &position);
  }
  if (status == CALL_OK && mode->truncate) {
    /* The wrapper, slot and open object are already reserved. Publishing a
     * successfully truncated stream cannot allocate or otherwise fail. */
    status = file_resize(handle, 0);
  }
  if (status != CALL_OK) {
    int error = libc_path_errno(status, path, NULL);
    if (handle != HANDLE_INVALID) {
      descriptor_release_handle(handle);
    }
    free(response);
    free(object);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(error);
  }
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1, .allocated = true, .kind = DESCRIPTOR_FILE, .handle = handle,
    .response = response,
    .position = position, .readable = mode->readable, .writable = mode->writable,
    .append = mode->append,
  };
  publish(descriptor, (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  });
  return descriptor;
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

int descriptor_stream(FILE *stream, struct startup_stream *binding)
{
  libc_lock_acquire(&table_guard);
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  struct descriptor_object *object = lookup(descriptor);
  int error = !object || entries[descriptor].stream != stream ? EBADF :
      object->http && !object->http->committed && object->kind == DESCRIPTOR_RESPONSE ? EBUSY : 0;
  if (!error) {
    *binding = (struct startup_stream){
      .protocol = object->kind == DESCRIPTOR_FILE ? PROTOCOL_FILE :
          object->kind == DESCRIPTOR_PIPE ? PROTOCOL_PIPE :
          object->kind == DESCRIPTOR_RESPONSE ? PROTOCOL_STREAM : PROTOCOL_CONSOLE,
      .handle = object->handle,
      .observation_protocol = object->kind == DESCRIPTOR_RESPONSE && object->observation ? PROTOCOL_HTTP_TRANSACTION : 0,
      .observation = object->kind == DESCRIPTOR_RESPONSE ? object->observation : HANDLE_INVALID,
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
    .access = (object->readable && (!object->http || object->http->committed) ? PYXIS_DESCRIPTOR_READ : 0) |
        (object->writable ? PYXIS_DESCRIPTOR_WRITE : 0),
    .buffered_read = object->ahead_count,
    .observation = object->kind == DESCRIPTOR_RESPONSE ? object->observation : HANDLE_INVALID,
  };
  unlock_object(&object->read_guard);
  enum call_status status = handle_query(result.handle, &result.info);
  descriptor_drop(object);
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  if (result.observation) {
    status = handle_query(result.observation, &result.observation_info);
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
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
static int release_alias(struct descriptor_object *object, bool final)
{
  int result = 0;
  if (final) {
    handle_t handles[6] = {object->handle, object->observation};
    if (object->http) {
      struct http_prepared *request = &object->http->prepared;
      handles[2] = request->control;
      handles[3] = request->body;
      handles[4] = request->observation;
      handles[5] = request->response;
    }
    int first_error = 0;
    for (size_t i = 0; i < 6; ++i) {
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

static int close_entry(int descriptor, FILE *stream, struct descriptor_object *expected)
{
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(descriptor);
  if (!object || (stream && (entries[descriptor].stream != stream || object != expected))) {
    libc_lock_release(&table_guard);
    return fail(EBADF);
  }
  if (entries[descriptor].stream) {
    __atomic_store_n(&entries[descriptor].stream->descriptor, -1, __ATOMIC_RELEASE);
  }
  install_locked(descriptor, (struct descriptor_entry){0});
  bool final = --object->references == 0;
  if (final) {
    __atomic_store_n(&object->closed, true, __ATOMIC_RELEASE);
  }
  libc_lock_release(&table_guard);
  /* Never wait on cursor/FILE locks before releasing native grant authority. */
  return release_alias(object, final);
}

int descriptor_close(int descriptor)
{
  return close_entry(descriptor, NULL, NULL);
}

int descriptor_close_stream(FILE *stream, struct descriptor_object *object)
{
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  return close_entry(descriptor, stream, object);
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
  FILE *stream = entries[target].stream;
  if (stream && !stream_retain_association(stream)) {
    libc_lock_release(&table_guard);
    descriptor_drop(held);
    return fail(EBUSY);
  }
  libc_lock_release(&table_guard);
  if (stream && !stream_try_lock(stream)) {
    stream_release(stream);
    descriptor_drop(held);
    return fail(EBUSY);
  }
  libc_lock_acquire(&table_guard);
  struct descriptor_object *object = lookup(source);
  int error = !object || object != held || entries[source].generation != source_generation ?
      EBADF : entries[target].generation != target_generation ||
      entries[target].state == DESCRIPTOR_RESERVED || entries[target].stream != stream || (stream && stream->output_count) ? EBUSY :
      __atomic_load_n(&object->storage, __ATOMIC_RELAXED) == SIZE_MAX ? EMFILE : 0;
  struct descriptor_object *previous = NULL;
  bool final = false;
  if (!error) {
    ++object->references;
    __atomic_add_fetch(&object->storage, 1, __ATOMIC_RELAXED);
    if (entries[target].state == DESCRIPTOR_OPEN) {
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
  if (!error && previous && release_alias(previous, final) < 0) {
    error = errno;
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
    release_alias(object, last);
  }
  if (stream) {
    stream_unlock(stream);
    stream_release(stream);
  }
  descriptor_drop(held);
  return error ? fail(error) : target;
}

void descriptor_finish(void)
{
  /* Orderly finalizers require joined workers and closed thread admission. */
  for (size_t i = 0; i < capacity; ++i) {
    if (entries[i].state == DESCRIPTOR_OPEN) {
      descriptor_close((int)i);
    }
  }
  if (entries != startup_entries) {
    free(entries);
  }
  entries = startup_entries;
  capacity = STARTUP_STREAM_COUNT;
  memset(startup_entries, 0, sizeof(startup_entries));
}

/* One backend transfer following any read-ahead; does not move the position. */
static int backend_read(struct descriptor_object *entry, void *buffer, size_t size,
    size_t *read)
{
  enum call_status status;
  if (entry->http && !entry->http->committed && entry->readable) {
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
      entry->final_error = libc_call_errno(status);
      return fail(entry->final_error);
    }
    if (status == CALL_OK && !*read) {
      struct http_transaction_status final;
      status = http_transaction_status(entry->observation, 0, &final);
      entry->final_checked = true;
      if (status != CALL_OK || final.framing != HTTP_FRAMING_COMPLETE ||
          final.phase != HTTP_TRANSACTION_COMPLETE || final.status != CALL_OK) {
        entry->final_error = status != CALL_OK ? libc_call_errno(status) : EIO;
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
    status = pipe_read(entry->handle, buffer, size, read);
  } else {
    status = console_read(entry->handle, buffer, size, read);
  }
  if (status != CALL_OK) {
    *read = 0;
    return fail(libc_call_errno(status));
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
  enum call_status status = entry->kind == DESCRIPTOR_PIPE ?
      pipe_try_read(entry->handle, buffer, count, &read) :
      console_read_timeout(entry->handle, buffer, count, 0, &read);
  if (status != CALL_OK) {
    return fail(status == CALL_TIMED_OUT ? EAGAIN : libc_call_errno(status));
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
    if (entry->http) {
      entry->http->producer_failed = true;
    }
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    if (entry->http) {
      entry->http->producer_failed = true;
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
    if (entry->http) {
      entry->http->producer_failed = true;
    }
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    if (entry->http) {
      entry->http->producer_failed = true;
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
    if (entry->http) {
      entry->http->producer_failed = true;
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

static int response_locked(struct descriptor_object *entry, struct pyxis_response_info *info)
{
  if (!entry) {
    return -1;
  }
  if (entry->response && entry->response->flags) {
    *info = *entry->response;
    return 0;
  }
  if (entry->observation && entry->kind == DESCRIPTOR_RESPONSE) {
    struct http_response_metadata metadata;
    struct http_transaction_status response;
    enum call_status status = http_transaction_metadata(entry->observation, 0, &metadata);
    if (status == CALL_OK) {
      status = http_transaction_status(entry->observation, 0, &response);
    }
    if (status != CALL_OK) {
      return fail(libc_call_errno(status));
    }
    info->flags = PYXIS_RESPONSE_PROVIDER | PYXIS_RESPONSE_HTTP | PYXIS_RESPONSE_URL;
    info->status = response.http_status;
    memcpy(info->url, metadata.uri, sizeof(info->url));
    if (metadata.media_type[0]) {
      info->flags |= PYXIS_RESPONSE_MEDIA_TYPE;
      memcpy(info->media_type, metadata.media_type, sizeof(info->media_type));
    }
  }
  return 0;
}

/* Operation locks serialize mutable state, never native grant ownership. */
static struct libc_mutex *object_guard(struct descriptor_object *object, bool writing)
{
  return writing && object->kind != DESCRIPTOR_FILE && !object->http ? &object->write_guard : &object->read_guard;
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

int descriptor_object_stream(struct descriptor_object *object, struct startup_stream *binding)
{
  if (!object || __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    return fail(EBADF);
  }
  if (object->http && !object->http->committed && object->kind == DESCRIPTOR_RESPONSE) {
    return fail(EBUSY);
  }
  *binding = (struct startup_stream){
    .protocol = object->kind == DESCRIPTOR_FILE ? PROTOCOL_FILE :
        object->kind == DESCRIPTOR_PIPE ? PROTOCOL_PIPE :
          object->kind == DESCRIPTOR_RESPONSE ? PROTOCOL_STREAM : PROTOCOL_CONSOLE,
    .handle = object->handle,
    .observation_protocol = object->kind == DESCRIPTOR_RESPONSE && object->observation ? PROTOCOL_HTTP_TRANSACTION : 0,
    .observation = object->kind == DESCRIPTOR_RESPONSE ? object->observation : HANDLE_INVALID,
  };
  return 0;
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

int descriptor_object_response(struct descriptor_object *object, struct pyxis_response_info *info)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  int result = response_locked(object, info);
  unlock_object(object_guard(object, false));
  return result;
}

int descriptor_response(int descriptor, struct pyxis_response_info *info)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  int result = descriptor_object_response(object, info);
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
  if (!object || (object->kind != DESCRIPTOR_FILE && !object->http) ||
      __atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
    return false;
  }
  return object_enter(object, false) == 0;
}

void descriptor_file_end(struct descriptor_object *object)
{
  unlock_object(&object->read_guard);
}

int descriptor_http_open(const char *path, const struct descriptor_mode *mode,
    FILE *stream, const struct pyxis_http_options *options, bool submit)
{
  if (!path || mode->append || mode->exclusive || (mode->writable && !mode->truncate)) {
    return fail(EINVAL);
  }
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  struct descriptor_object *object = calloc(1, sizeof(*object));
  struct descriptor_http *http = calloc(1, sizeof(*http));
  struct pyxis_response_info *info = calloc(1, sizeof(*info));
  if (!object || !http || !info) {
    free(object);
    free(http);
    free(info);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(ENOMEM);
  }
  uint64_t deadline = options ? options->deadline_ns : 0;
  enum call_status status = CALL_OK;
  if (!deadline) {
    uint64_t now;
    status = clock_now(startup_resource("clock"), &now);
    if (status == CALL_OK) {
      if (now > UINT64_MAX - UINT64_C(30000000000)) {
        status = CALL_LIMIT;
      } else {
        deadline = now + UINT64_C(30000000000);
      }
    }
  }
  const struct path_context *context = NULL;
  if (status == CALL_OK) {
    status = pyxis_working_context(&context);
  }
  uint64_t method = mode->writable ? HTTP_PREPARE_POST : HTTP_PREPARE_GET;
  if (status == CALL_OK) {
    status = submit ? http_transaction_open_uri(context, path, deadline, &http->prepared, info) :
        http_transaction_prepare_uri(context, path, method, options ? options->flags : 0,
            options ? options->headers : NULL, options ? options->headers_size : 0,
            deadline, &http->prepared);
  }
  if (status != CALL_OK) {
    http_prepared_close(&http->prepared);
    free(object);
    free(http);
    free(info);
    publish(descriptor, (struct descriptor_entry){0});
    return fail(libc_path_errno(status, path, NULL));
  }
  http->deadline = deadline;
  http->inplace = mode->readable;
  http->committed = submit;
  http->attempted = submit;
  *object = (struct descriptor_object){
    .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
    .references = 1, .storage = 1, .allocated = true,
    .kind = method == HTTP_PREPARE_POST ? DESCRIPTOR_FILE : DESCRIPTOR_RESPONSE,
    .handle = submit ? http->prepared.response : http->prepared.body,
    .observation = http->prepared.observation, .http = http, .response = info,
    .readable = mode->readable, .writable = mode->writable,
  };
  publish(descriptor, (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  });
  return descriptor;
}

bool descriptor_object_http_inplace(struct descriptor_object *object)
{
  return object && object->http && object->http->inplace;
}

int descriptor_object_http_status(struct descriptor_object *object,
    struct http_transaction_status *status)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  enum call_status result = object->observation ?
      http_transaction_status(object->observation, 0, status) : CALL_BAD_OPERATION;
  unlock_object(&object->read_guard);
  return result == CALL_OK ? 0 : fail(libc_call_errno(result));
}

int descriptor_object_http_abort(struct descriptor_object *object)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  enum call_status result = object->http ?
      http_transaction_abort(object->http->prepared.control, 0) : CALL_DENIED;
  unlock_object(&object->read_guard);
  return result == CALL_OK ? 0 : fail(libc_call_errno(result));
}

int descriptor_object_http_commit(struct descriptor_object *object, FILE *response_stream)
{
  if (object_enter(object, false) < 0) {
    return -1;
  }
  struct descriptor_http *http = object->http;
  if (!http || http->attempted || http->producer_failed) {
    int error = !http ? ENOTSUP : http->producer_failed ? EIO : EBUSY;
    unlock_object(&object->read_guard);
    return fail(error);
  }
  int descriptor = -1;
  struct descriptor_object *response = NULL;
  struct pyxis_response_info *info = NULL;
  handle_t observation = HANDLE_INVALID;
  enum call_status status = CALL_OK;
  if (!http->inplace) {
    descriptor = reserve();
    if (descriptor < 0) {
      unlock_object(&object->read_guard);
      return -1;
    }
    response = calloc(1, sizeof(*response));
    info = calloc(1, sizeof(*info));
    status = !response || !info ? CALL_NO_MEMORY :
        handle_copy_restricted(object->observation, HTTP_TRANSACTION_RIGHT_STATUS,
            HANDLE_TRANSPORT_CALL, &observation);
  }
  struct http_prepared prepared = http->prepared;
  struct http_commit_reply reply;
  if (status == CALL_OK) {
    /* Even a lost reply cannot lead to a second submission from this object. */
    http->attempted = true;
    object->writable = false;
    status = http_transaction_commit(&prepared, http->deadline, &reply);
    if (status == CALL_OK && !(reply.flags & HTTP_COMMIT_RESPONSE)) {
      status = CALL_OUTCOME_UNKNOWN;
    }
  }
  if (status == CALL_OK) {
    libc_lock_acquire(&table_guard);
    if (__atomic_load_n(&object->closed, __ATOMIC_ACQUIRE)) {
      status = CALL_BAD_HANDLE;
    } else if (http->inplace) {
      http->committed = true;
      http->prepared.response = prepared.response;
      object->handle = prepared.response;
      object->kind = DESCRIPTOR_RESPONSE;
      object->writable = false;
      object->readable = true;
      discard_ahead(object);
      object->position = 0;
    } else {
      http->committed = true;
      object->writable = false;
      *response = (struct descriptor_object){
        .read_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
        .write_guard = LIBC_RECURSIVE_MUTEX_INITIALIZER,
        .references = 1, .storage = 1, .allocated = true, .kind = DESCRIPTOR_RESPONSE,
        .handle = prepared.response, .observation = observation, .response = info,
        .readable = true,
      };
      observation = HANDLE_INVALID;
    }
    libc_lock_release(&table_guard);
  }
  if (status != CALL_OK) {
    if (http->attempted) {
      http->failure = libc_call_errno(status);
    }
    if (prepared.response) {
      descriptor_release_handle(prepared.response);
    }
    if (observation) {
      descriptor_release_handle(observation);
    }
    free(info);
    free(response);
    if (descriptor >= 0) {
      publish(descriptor, (struct descriptor_entry){0});
    }
  } else if (!http->inplace) {
    publish(descriptor, (struct descriptor_entry){
      .state = DESCRIPTOR_OPEN, .object = response, .stream = response_stream,
    });
  }
  unlock_object(&object->read_guard);
  return status == CALL_OK ? 0 : fail(libc_call_errno(status));
}
