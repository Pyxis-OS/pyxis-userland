#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <console.h>
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

enum descriptor_state { DESCRIPTOR_FREE, DESCRIPTOR_RESERVED, DESCRIPTOR_OPEN };
enum descriptor_kind { DESCRIPTOR_CONSOLE, DESCRIPTOR_FILE, DESCRIPTOR_PIPE };

/* Each descriptor owns one reference to an open object. Duplication shares its
 * native handle, access, append policy, logical position and unread read-ahead.
 * Position is the next byte delivered; backend reads start after read-ahead. */
struct descriptor_object {
  size_t references;
  bool allocated;
  enum descriptor_kind kind;
  handle_t handle;
  uint64_t position;
  bool readable, writable, append;
  bool unbuffered; /* Read-ahead allocation failed; stay exact until close. */
  unsigned char *ahead; /* Owned, BUFSIZ bytes, allocated on first fill. */
  size_t ahead_start, ahead_count;
  struct pyxis_response_info *response; /* Owned metadata for this open. */
};

struct descriptor_entry {
  enum descriptor_state state;
  struct descriptor_object *object;
  FILE *stream; /* Non-owning; only one FILE can associate with this slot. */
};

static struct descriptor_object startup_objects[STARTUP_STREAM_COUNT];
static struct descriptor_entry startup_entries[STARTUP_STREAM_COUNT];
static struct descriptor_entry *entries = startup_entries;
static size_t capacity = STARTUP_STREAM_COUNT;

static int fail(int error)
{
  errno = error;
  return -1;
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

static int reserve(void)
{
  size_t index;
  for (index = 0; index < capacity; ++index) {
    if (entries[index].state == DESCRIPTOR_FREE) {
      entries[index].state = DESCRIPTOR_RESERVED;
      return (int)index;
    }
  }

  size_t limit = (size_t)INT_MAX + 1;
  if (limit > SIZE_MAX / sizeof(*entries)) {
    limit = SIZE_MAX / sizeof(*entries);
  }
  if (capacity == limit) {
    return fail(EMFILE);
  }
  size_t next = capacity > limit / 2 ? limit : capacity * 2;
  struct descriptor_entry *grown = malloc(next * sizeof(*entries));
  if (!grown) {
    return -1;
  }
  memcpy(grown, entries, capacity * sizeof(*entries));
  memset(grown + capacity, 0, (next - capacity) * sizeof(*entries));
  if (entries != startup_entries) {
    free(entries);
  }
  entries = grown;
  capacity = next;
  entries[index].state = DESCRIPTOR_RESERVED;
  return (int)index;
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
    .references = 1,
    .kind = binding.protocol == PROTOCOL_FILE ? DESCRIPTOR_FILE :
            binding.protocol == PROTOCOL_PIPE ? DESCRIPTOR_PIPE : DESCRIPTOR_CONSOLE,
    .handle = binding.handle,
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
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  struct descriptor_object *object = malloc(sizeof(*object));
  if (!object) {
    entries[descriptor] = (struct descriptor_entry){0};
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
    entries[descriptor] = (struct descriptor_entry){0};
    return fail(error);
  }
  *object = (struct descriptor_object){
    .references = 1, .allocated = true, .kind = DESCRIPTOR_FILE, .handle = handle,
    .response = response,
    .position = position, .readable = mode->readable, .writable = mode->writable,
    .append = mode->append,
  };
  entries[descriptor] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  };
  if (stream) {
    stream->descriptor = descriptor;
  }
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
    entries[descriptor] = (struct descriptor_entry){0};
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
    entries[descriptor] = (struct descriptor_entry){0};
    return fail(error);
  }
  *object = (struct descriptor_object){
    .references = 1, .allocated = true, .kind = DESCRIPTOR_FILE, .handle = handle,
    .readable = true, .writable = true,
  };
  entries[descriptor] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object, .stream = stream,
  };
  stream->descriptor = descriptor;
  return descriptor;
}

int descriptor_stream(FILE *stream, struct startup_stream *binding)
{
  struct descriptor_object *entry = lookup(stream->descriptor);
  if (!entry) {
    return -1;
  }
  if (entries[stream->descriptor].stream != stream) {
    return fail(EBADF);
  }
  *binding = (struct startup_stream){
    .protocol = entry->kind == DESCRIPTOR_FILE ? PROTOCOL_FILE :
        entry->kind == DESCRIPTOR_PIPE ? PROTOCOL_PIPE : PROTOCOL_CONSOLE,
    .handle = entry->handle,
  };
  return 0;
}

int pyxis_descriptor_borrow(int descriptor, struct pyxis_descriptor_binding *binding)
{
  if (!binding) {
    return fail(EINVAL);
  }
  *binding = (struct pyxis_descriptor_binding){0};
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return -1;
  }
  struct pyxis_descriptor_binding result = {
    .handle = entry->handle,
    .access = (entry->readable ? PYXIS_DESCRIPTOR_READ : 0) |
        (entry->writable ? PYXIS_DESCRIPTOR_WRITE : 0),
    .buffered_read = entry->ahead_count,
  };
  enum call_status status = handle_query(result.handle, &result.info);
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
  for (size_t i = 0; i < capacity; ++i) {
    if (entries[i].state == DESCRIPTOR_OPEN && entries[i].object->handle == *handle) {
      return fail(EBUSY);
    }
  }
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
    entries[descriptor] = (struct descriptor_entry){0};
    return -1;
  }
  *object = (struct descriptor_object){
    .references = 1, .allocated = true, .kind = kind, .handle = *handle,
    .readable = access & PYXIS_DESCRIPTOR_READ,
    .writable = access & PYXIS_DESCRIPTOR_WRITE,
  };
  entries[descriptor] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object,
  };
  *handle = HANDLE_INVALID;
  return descriptor;
}

bool descriptor_ready(int descriptor, bool writing)
{
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return false;
  }
  if (writing ? !entry->writable : !entry->readable) {
    errno = EBADF;
    return false;
  }
  return true;
}

int descriptor_associate(int descriptor, const struct descriptor_mode *mode, FILE *stream)
{
  struct descriptor_object *object = lookup(descriptor);
  if (!object) {
    return -1;
  }
  if ((mode->readable && !object->readable) || (mode->writable && !object->writable)) {
    return fail(EBADF);
  }
  if (entries[descriptor].stream) {
    return fail(EBUSY);
  }
  /* Append is shared open-object policy. Selecting a enables it for every
   * duplicate without changing the cursor; other modes never turn it off. */
  if (mode->append) {
    object->append = true;
  }
  entries[descriptor].stream = stream;
  stream->descriptor = descriptor;
  return 0;
}

int dup(int descriptor)
{
  struct descriptor_object *object = lookup(descriptor);
  if (!object) {
    return -1;
  }
  int copy = reserve();
  if (copy < 0) {
    return -1;
  }
  ++object->references;
  entries[copy] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .object = object,
  };
  return copy;
}

int descriptor_close(int descriptor)
{
  struct descriptor_object *object = lookup(descriptor);
  if (!object) {
    return -1;
  }
  if (entries[descriptor].stream) {
    entries[descriptor].stream->descriptor = -1;
  }
  entries[descriptor] = (struct descriptor_entry){0};
  if (--object->references) {
    return 0;
  }
  handle_t handle = object->handle;
  free(object->ahead);
  free(object->response);
  if (object->allocated) {
    free(object);
  } else {
    *object = (struct descriptor_object){0};
  }
  /* Never retry an uncertain release or reconnect a FILE after slot reuse.
   * Any residual native entry is reclaimed by kernel process teardown. */
  return descriptor_release_handle(handle);
}

void descriptor_finish(void)
{
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
  if (entry->kind == DESCRIPTOR_FILE) {
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

static struct descriptor_object *try_entry(int descriptor, size_t count, bool writing)
{
  if (!descriptor_ready(descriptor, writing)) {
    return NULL;
  }
  if (count > (size_t)SSIZE_MAX) {
    errno = EINVAL;
    return NULL;
  }
  struct descriptor_object *entry = entries[descriptor].object;
  if (entry->kind == DESCRIPTOR_FILE) {
    errno = ENOTSUP;
    return NULL;
  }
  return entry;
}

ssize_t pyxis_descriptor_try_read(int descriptor, void *buffer, size_t count)
{
  struct descriptor_object *entry = try_entry(descriptor, count, false);
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

ssize_t pyxis_descriptor_try_write(int descriptor, const void *buffer, size_t count)
{
  struct descriptor_object *entry = try_entry(descriptor, count, true);
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

int descriptor_read(int descriptor, void *buffer, size_t size, size_t *read)
{
  *read = 0;
  if (!descriptor_ready(descriptor, false)) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  struct descriptor_object *entry = entries[descriptor].object;
  if (entry->ahead_count) {
    *read = take_ahead(entry, buffer, size);
    return 0;
  }
  return read_exact(entry, buffer, size, read);
}

int descriptor_read_complete(int descriptor, void *buffer, size_t size, size_t *read)
{
  if (descriptor_read(descriptor, buffer, size, read) < 0) {
    return -1;
  }
  while (*read < size && entries[descriptor].object->kind == DESCRIPTOR_FILE) {
    size_t more;
    if (descriptor_read(descriptor, (char *)buffer + *read, size - *read, &more) < 0) {
      return 0;
    }
    if (!more) {
      break;
    }
    *read += more;
  }
  return 0;
}

int descriptor_read_buffered(int descriptor, void *buffer, size_t size, size_t *read)
{
  *read = 0;
  if (!descriptor_ready(descriptor, false)) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  struct descriptor_object *entry = entries[descriptor].object;
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

void descriptor_discard_input(int descriptor)
{
  if (descriptor < 0 || (size_t)descriptor >= capacity ||
      entries[descriptor].state != DESCRIPTOR_OPEN ||
      entries[descriptor].object->kind != DESCRIPTOR_FILE) {
    return;
  }
  discard_ahead(entries[descriptor].object);
}

int descriptor_write(int descriptor, const void *buffer, size_t size, size_t *written)
{
  *written = 0;
  if (!descriptor_ready(descriptor, true)) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  struct descriptor_object *entry = entries[descriptor].object;
  /* Files can refetch speculative bytes at the shared logical position.
   * Pipe bytes already consumed from the backend must remain available. */
  if (entry->kind == DESCRIPTOR_FILE) {
    discard_ahead(entry);
  }
  uint64_t position = entry->position;
  enum call_status status;
  if (entry->kind == DESCRIPTOR_FILE) {
    if (entry->append) {
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
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    *written = 0;
    return fail(EIO);
  }
  if (entry->kind == DESCRIPTOR_FILE) {
    entry->position = position + *written;
  }
  return 0;
}

int descriptor_pread(int descriptor, void *buffer, size_t size, uint64_t offset,
    size_t *read)
{
  *read = 0;
  if (!descriptor_ready(descriptor, false)) {
    return -1;
  }
  struct descriptor_object *entry = entries[descriptor].object;
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

int descriptor_pwrite(int descriptor, const void *buffer, size_t size, uint64_t offset,
    size_t *written)
{
  *written = 0;
  if (!descriptor_ready(descriptor, true)) {
    return -1;
  }
  struct descriptor_object *entry = entries[descriptor].object;
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
    return fail(libc_call_errno(status));
  }
  if (!*written || *written > size) {
    *written = 0;
    return fail(EIO);
  }
  return 0;
}

int descriptor_seek(int descriptor, long offset, int origin)
{
  struct descriptor_object *entry = lookup(descriptor);
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

int descriptor_resize(int descriptor, uint64_t size)
{
  struct descriptor_object *entry = lookup(descriptor);
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
    return fail(libc_call_errno(status));
  }
  return 0;
}

int descriptor_sync(int descriptor)
{
  struct descriptor_object *entry = lookup(descriptor);
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

int descriptor_stat(int descriptor, struct stat *result)
{
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return -1;
  }
  if (entry->kind == DESCRIPTOR_CONSOLE) {
    *result = (struct stat){.st_mode = S_IFCHR};
    return 0;
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

int descriptor_terminal(int descriptor)
{
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return 0;
  }
  if (entry->kind != DESCRIPTOR_CONSOLE) {
    errno = ENOTTY;
    return 0;
  }
  return 1;
}

long descriptor_tell(int descriptor)
{
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return -1;
  }
  if (entry->kind != DESCRIPTOR_FILE) {
    return fail(ESPIPE);
  }
  if (entry->position > LONG_MAX) {
    return fail(EOVERFLOW);
  }
  return (long)entry->position;
}

int descriptor_response(int descriptor, struct pyxis_response_info *info)
{
  struct descriptor_object *entry = lookup(descriptor);
  if (!entry) {
    return -1;
  }
  if (entry->response) {
    *info = *entry->response;
  }
  return 0;
}
