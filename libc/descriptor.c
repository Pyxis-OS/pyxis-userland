#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <console.h>
#include <errno.h>
#include <file.h>
#include <limits.h>
#include <pipe.h>
#include <startup.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include "descriptor.h"
#include "errors.h"
#include "stream.h"

enum descriptor_state { DESCRIPTOR_FREE, DESCRIPTOR_RESERVED, DESCRIPTOR_OPEN };
enum descriptor_kind { DESCRIPTOR_CONSOLE, DESCRIPTOR_FILE, DESCRIPTOR_PIPE };

struct descriptor_entry {
  enum descriptor_state state;
  enum descriptor_kind kind;
  handle_t handle;
  uint64_t position;
  bool readable, writable, append;
  FILE *stream; /* Non-owning; only one FILE can associate with an entry. */
};

static struct descriptor_entry startup_entries[STARTUP_STREAM_COUNT];
static struct descriptor_entry *entries = startup_entries;
static size_t capacity = STARTUP_STREAM_COUNT;

static int fail(int error)
{
  errno = error;
  return -1;
}

static struct descriptor_entry *lookup(int descriptor)
{
  if (descriptor < 0 || (size_t)descriptor >= capacity ||
      entries[descriptor].state != DESCRIPTOR_OPEN) {
    errno = EBADF;
    return NULL;
  }
  return &entries[descriptor];
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

static int release_handle(handle_t handle)
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
  entries[index] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN,
    .kind = binding.protocol == PROTOCOL_FILE ? DESCRIPTOR_FILE :
            binding.protocol == PROTOCOL_PIPE ? DESCRIPTOR_PIPE : DESCRIPTOR_CONSOLE,
    .handle = binding.handle,
    .readable = index == STARTUP_STDIN,
    .writable = index != STARTUP_STDIN,
    .stream = stream,
  };
  stream->descriptor = (int)index;
}

int descriptor_open(const char *path, const struct descriptor_mode *mode, FILE *stream)
{
  int descriptor = reserve();
  if (descriptor < 0) {
    return -1;
  }
  handle_t handle = HANDLE_INVALID;
  uint64_t position = 0;
  uint64_t rights = (mode->readable ? FILE_RIGHT_READ : 0) |
                    (mode->writable ? FILE_RIGHT_WRITE : 0);
  enum call_status status = file_open_path(path, rights, mode->create, &handle);
  if (status == CALL_OK && mode->append && !mode->readable) {
    status = file_size(handle, &position);
  }
  if (status == CALL_OK && mode->truncate) {
    /* The caller allocated its FILE and reserve secured the slot. Publishing
     * a successfully truncated stream cannot allocate or otherwise fail. */
    status = file_resize(handle, 0);
  }
  if (status != CALL_OK) {
    int error = libc_call_errno(status);
    if (handle != HANDLE_INVALID) {
      release_handle(handle);
    }
    entries[descriptor] = (struct descriptor_entry){0};
    return fail(error);
  }
  entries[descriptor] = (struct descriptor_entry){
    .state = DESCRIPTOR_OPEN, .kind = DESCRIPTOR_FILE, .handle = handle,
    .position = position, .readable = mode->readable, .writable = mode->writable,
    .append = mode->append, .stream = stream,
  };
  if (stream) {
    stream->descriptor = descriptor;
  }
  return descriptor;
}

bool descriptor_ready(int descriptor, bool writing)
{
  struct descriptor_entry *entry = lookup(descriptor);
  if (!entry) {
    return false;
  }
  if (writing ? !entry->writable : !entry->readable) {
    errno = EBADF;
    return false;
  }
  return true;
}

int descriptor_close(int descriptor)
{
  struct descriptor_entry *entry = lookup(descriptor);
  if (!entry) {
    return -1;
  }
  handle_t handle = entry->handle;
  if (entry->stream) {
    entry->stream->descriptor = -1;
  }
  *entry = (struct descriptor_entry){0};
  /* Never retry an uncertain release or reconnect a FILE after slot reuse.
   * Any residual native entry is reclaimed by kernel process teardown. */
  return release_handle(handle);
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

int descriptor_read(int descriptor, void *buffer, size_t size, size_t *read)
{
  *read = 0;
  if (!descriptor_ready(descriptor, false)) {
    return -1;
  }
  if (!size) {
    return 0;
  }
  struct descriptor_entry *entry = &entries[descriptor];
  enum call_status status;
  if (entry->kind == DESCRIPTOR_FILE) {
    if (size > UINT64_MAX - entry->position) {
      return fail(EOVERFLOW);
    }
    status = file_read(entry->handle, entry->position, buffer, size, read);
  } else if (entry->kind == DESCRIPTOR_PIPE) {
    status = pipe_read(entry->handle, buffer, size, read);
  } else {
    status = console_read(entry->handle, buffer, size, read);
  }
  if (status != CALL_OK) {
    return fail(libc_call_errno(status));
  }
  if (*read > size || (!*read && entry->kind == DESCRIPTOR_CONSOLE)) {
    *read = 0;
    return fail(EIO);
  }
  if (entry->kind == DESCRIPTOR_FILE) {
    entry->position += *read;
  }
  return 0;
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
  struct descriptor_entry *entry = &entries[descriptor];
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

int descriptor_seek(int descriptor, long offset, int origin)
{
  struct descriptor_entry *entry = lookup(descriptor);
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
  entry->position = offset < 0 ? base - distance : base + distance;
  return 0;
}

long descriptor_tell(int descriptor)
{
  struct descriptor_entry *entry = lookup(descriptor);
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
