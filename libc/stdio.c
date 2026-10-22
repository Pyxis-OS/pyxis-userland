#include <environment.h>
#include <abi/directory.h>
#include <abi/file.h>
#include <errno.h>
#include <handle.h>
#include <pipe.h>
#include <limits.h>
#include <stdint.h>
#include <stdatomic.h>
#include <startup.h>
#include <pyxis/descriptor.h>
#include <pyxis/stdio.h>
#include <pyxis/resource.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "errors.h"
#include "runtime.h"
#include "stream.h"
#include "thread_sync.h"
#include "thread_platform.h"

static FILE standard_input, standard_output, standard_error;
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;
static FILE *streams;
static struct libc_lock stream_guard;

bool stream_retain(FILE *stream)
{
  libc_lock_acquire(&stream_guard);
  FILE *current = streams;
  while (current && current != stream) {
    current = current->next;
  }
  bool retained = current && !current->closing && current->references != UINT32_MAX;
  if (retained) {
    ++current->references;
  }
  libc_lock_release(&stream_guard);
  return retained;
}

bool stream_retain_association(FILE *stream)
{
  if (atomic_load_explicit(&stream->closing, memory_order_acquire)) {
    return false;
  }
  uint32_t count = atomic_load_explicit(&stream->references, memory_order_relaxed);
  while (count != UINT32_MAX) {
    if (atomic_compare_exchange_weak_explicit(&stream->references, &count, count + 1,
        memory_order_relaxed, memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void stream_release(FILE *stream)
{
  libc_lock_acquire(&stream_guard);
  --stream->references;
  bool dispose = stream->closing && !stream->references && stream->allocated;
  libc_lock_release(&stream_guard);
  if (dispose) {
    free(stream);
  }
}

bool stream_try_lock(FILE *stream)
{
  int error = libc_mutex_trylock(&stream->lock);
  if (!error && stream->operation_depth) {
    stream_unlock(stream);
    error = EBUSY;
  }
  if (error) {
    errno = error;
  }
  return error == 0;
}

void stream_unlock(FILE *stream)
{
  if (libc_mutex_unlock(&stream->lock)) {
    __builtin_trap();
  }
}

static bool stream_acquire_retained(FILE *stream)
{
  int error = libc_mutex_lock(&stream->lock);
  if (error) {
    stream_release(stream);
    errno = error;
    return false;
  }
  uint64_t self = libc_thread_self();
  if (self && __atomic_load_n(&stream->operation_owner, __ATOMIC_ACQUIRE) == self) {
    ++stream->operation_depth;
    stream_release(stream); /* The outer FILE admission owns recursive uses. */
    return true;
  }
  stream->operation_depth = 1;
  __atomic_store_n(&stream->operation_owner, libc_thread_self(), __ATOMIC_RELEASE);
  int saved = errno;
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  stream->operation = descriptor >= 0 ? descriptor_hold(descriptor) : NULL;
  stream->operation_cursor = descriptor_file_begin(stream->operation);
  errno = saved;
  return true;
}

bool stream_acquire(FILE *stream)
{
  uint64_t self = libc_thread_self();
  libc_lock_acquire(&stream_guard);
  FILE *current = streams;
  while (current && current != stream) {
    current = current->next;
  }
  bool recursive = current && self &&
      __atomic_load_n(&current->operation_owner, __ATOMIC_ACQUIRE) == self;
  if (!current || (!recursive && (current->closing || current->references == UINT32_MAX))) {
    libc_lock_release(&stream_guard);
    errno = EBADF;
    return false;
  }
  if (!recursive) {
    ++current->references;
  }
  libc_lock_release(&stream_guard);
  if (recursive) {
    int error = libc_mutex_lock(&stream->lock);
    if (error) {
      errno = error;
      return false;
    }
    ++stream->operation_depth;
    return true;
  }
  return stream_acquire_retained(stream);
}

void stream_end(FILE *stream)
{
  int saved = errno;
  bool outer = --stream->operation_depth == 0;
  if (outer) {
    if (stream->operation_cursor) {
      descriptor_file_end(stream->operation);
      stream->operation_cursor = false;
    }
    descriptor_drop(stream->operation);
    stream->operation = NULL;
    __atomic_store_n(&stream->operation_owner, 0, __ATOMIC_RELEASE);
  }
  stream_unlock(stream);
  if (outer) {
    stream_release(stream);
  }
  errno = saved;
}

void stream_operation_end(struct stream_operation *operation)
{
  if (operation->stream) {
    stream_end(operation->stream);
  }
}

int stream_error(FILE *stream, int error)
{
  if (stream) {
    stream->error = true;
    stream->transaction_read_blocked = false;
  }
  errno = error;
  return EOF;
}

bool stream_ready(FILE *stream, bool writing)
{
  if (!stream || stream->closed) {
    stream_error(stream, EBADF);
    return false;
  }
  if (writing ? !stream->writable : !stream->readable) {
    stream_error(stream, EBADF);
    return false;
  }
  if (!descriptor_object_ready(stream->operation, writing)) {
    bool read_blocked = !writing && errno == EBUSY && (!stream->error || stream->transaction_read_blocked);
    stream_error(stream, errno);
    stream->transaction_read_blocked = read_blocked;
    return false;
  }
  return true;
}

static bool select_orientation(FILE *stream, int orientation)
{
  if (stream->orientation && stream->orientation != orientation) {
    stream_error(stream, EINVAL);
    return false;
  }
  stream->orientation = orientation;
  return true;
}

bool stream_byte_ready(FILE *stream, bool writing)
{
  return stream_ready(stream, writing) && select_orientation(stream, STREAM_BYTE_ORIENTED);
}

static size_t pushback_bytes(FILE *stream)
{
  return stream->has_pushback ? 1 :
      stream->has_wide_pushback ? stream->wide_pushback_bytes : 0;
}

static void discard_pushback(FILE *stream)
{
  stream->has_pushback = false;
  stream->has_wide_pushback = false;
}

static void register_stream(FILE *stream)
{
  libc_lock_acquire(&stream_guard);
  stream->next = streams;
  streams = stream;
  libc_lock_release(&stream_guard);
}

static void standard_stream(FILE *stream, enum environment_stream_index index, handle_t snapshot)
{
  *stream = (FILE){
    .lock = LIBC_RECURSIVE_MUTEX_INITIALIZER, .descriptor = -1, .readable = index == ENVIRONMENT_STDIN,
    .writable = index != ENVIRONMENT_STDIN,
  };
  struct pyxis_stdio_binding binding = {0};
  uint64_t identity = 0;
  enum call_status status = environment_stdio(snapshot, index, &binding, &identity);
  if (status != CALL_OK && status != CALL_NOT_FOUND) {
    libc_start_failed(PROCESS_START_STDIN + index, status);
  }
  descriptor_adopt_standard(index, stream, binding, identity);
  register_stream(stream);
}

void stdio_init(void)
{
  handle_t snapshot;
  enum call_status status = environment_snapshot(startup_environment_handle(),
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &snapshot, NULL);
  if (status != CALL_OK) {
    libc_start_failed(PROCESS_START_STDIO_SNAPSHOT, status);
  }
  standard_stream(stdin, ENVIRONMENT_STDIN, snapshot);
  standard_stream(stdout, ENVIRONMENT_STDOUT, snapshot);
  standard_stream(stderr, ENVIRONMENT_STDERR, snapshot);
  libc_startup_close(snapshot, PROCESS_START_STDIO_CLOSE);
}

static bool parse_mode(const char *text, struct descriptor_mode *mode)
{
  if (!text || (*text != 'r' && *text != 'w' && *text != 'a')) {
    return false;
  }
  *mode = (struct descriptor_mode){
    .readable = *text == 'r', .writable = *text != 'r',
    .append = *text == 'a', .create = *text != 'r', .truncate = *text == 'w',
  };
  bool binary = false, update = false;
  while (*++text) {
    if (*text == 'b' && !binary) {
      binary = true;
    } else if (*text == '+' && !update) {
      update = true;
      mode->readable = true;
      mode->writable = true;
    } else {
      return false;
    }
  }
  return true;
}

FILE *pyxis_fopen(const char *path, const char *mode, struct pyxis_open_result *result)
{
  if (result) {
    if (result->available || result->resource.resource != HANDLE_INVALID ||
        result->resource.info.error_body != HANDLE_INVALID) {
      errno = EBUSY;
      return NULL;
    }
    *result = (struct pyxis_open_result){0};
  }
  struct descriptor_mode options;
  if (!parse_mode(mode, &options)) {
    errno = EINVAL;
    return NULL;
  }
  FILE *stream = malloc(sizeof(*stream));
  if (!stream) {
    return NULL;
  }
  *stream = (FILE){
    .lock = LIBC_RECURSIVE_MUTEX_INITIALIZER, .descriptor = -1, .allocated = true,
    .readable = options.readable, .writable = options.writable,
  };
  if (descriptor_open_result(path, &options, stream, result) < 0) {
    free(stream);
    return NULL;
  }
  register_stream(stream);
  return stream;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
  return pyxis_fopen(path, mode, NULL);
}

int pyxis_open_result_close(struct pyxis_open_result *result)
{
  if (!result) {
    errno = EINVAL;
    return -1;
  }
  enum call_status status = resource_result_close(&result->resource);
  *result = (struct pyxis_open_result){0};
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  return 0;
}

/* Consumes an independent body handle, including failure unwinding. */
static FILE *open_error_body(handle_t body)
{
  if (body == HANDLE_INVALID) {
    errno = ENOENT;
    return NULL;
  }
  int descriptor = pyxis_descriptor_adopt(&body, PYXIS_DESCRIPTOR_READ);
  if (descriptor < 0) {
    int error = errno;
    handle_close(body);
    errno = error;
    return NULL;
  }
  FILE *stream = fdopen(descriptor, "r");
  if (!stream) {
    int error = errno;
    descriptor_close(descriptor);
    errno = error;
  }
  return stream;
}

FILE *pyxis_open_error_body(const struct pyxis_open_result *result)
{
  if (!result) {
    errno = EINVAL;
    return NULL;
  }
  if (!result->available || result->resource.info.error_body == HANDLE_INVALID) {
    errno = ENOENT;
    return NULL;
  }
  handle_t body;
  enum call_status status = handle_copy_restricted(result->resource.info.error_body,
      FILE_RIGHT_READ, 0, &body);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return NULL;
  }
  return open_error_body(body);
}

int pyxis_describe(FILE *stream, struct describe_reply *description)
{
  if (!description) {
    errno = EINVAL;
    return -1;
  }
  *description = (struct describe_reply){0};
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return descriptor_object_describe(stream->operation, 0, description);
}

FILE *pyxis_error_body(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return NULL;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  struct describe_reply description;
  if (descriptor_object_describe(stream->operation, DESCRIBE_INCLUDE_ERROR_BODY,
      &description) < 0) {
    return NULL;
  }
  return open_error_body(description.error_body);
}

FILE *fdopen(int descriptor, const char *mode)
{
  struct descriptor_mode options;
  if (!parse_mode(mode, &options)) {
    errno = EINVAL;
    return NULL;
  }
  FILE *stream = malloc(sizeof(*stream));
  if (!stream) {
    return NULL;
  }
  *stream = (FILE){
    .lock = LIBC_RECURSIVE_MUTEX_INITIALIZER, .descriptor = -1, .allocated = true,
    .readable = options.readable, .writable = options.writable,
  };
  /* Association has no fallible work after taking ownership. In particular,
   * w neither creates nor truncates, and no mode resets existing input. */
  if (descriptor_associate(descriptor, &options, stream) < 0) {
    free(stream);
    return NULL;
  }
  register_stream(stream);
  return stream;
}

FILE *tmpfile(void)
{
  int saved_errno = errno;
  FILE *stream = malloc(sizeof(*stream));
  if (!stream) {
    return NULL;
  }
  *stream = (FILE){
    .lock = LIBC_RECURSIVE_MUTEX_INITIALIZER, .descriptor = -1, .allocated = true, .readable = true, .writable = true,
  };
  handle_t parent;
  uint64_t rights = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES;
  /* Resolve an owned parent with removal authority before any creation. */
  enum call_status status = directory_open_path("tmp://", rights, &parent);
  if (status != CALL_OK) {
    free(stream);
    errno = libc_call_errno(status);
    return NULL;
  }
  int descriptor = descriptor_tmpfile(stream, parent);
  int error = errno;
  handle_close(parent);
  if (descriptor < 0) {
    free(stream);
    errno = error;
    return NULL;
  }
  register_stream(stream);
  errno = saved_errno;
  return stream;
}

int pyxis_stdio_stream(FILE *stream, struct pyxis_stdio_binding *binding)
{
  if (!binding) {
    errno = EINVAL;
    return -1;
  }
  *binding = (struct pyxis_stdio_binding){
    .protocol = 0, .handle = HANDLE_INVALID,
  };
  if (!stream) {
    errno = EINVAL;
    return -1;
  }
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  bool standard = stream == &standard_input || stream == &standard_output ||
      stream == &standard_error;
  if (standard && __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE) < 0) {
    return 0;
  }
  FILE *current = stream;
  if (current->closed) {
    errno = EBADF;
    return -1;
  }
  return descriptor_object_stream(current->operation, binding);
}

/* A capture producer, or the writer of a pipe that may carry values. */
struct handoff_target {
  handle_t handle;
  bool pipe;
};

static enum call_status handoff_destination(FILE *destination, struct handoff_target *target)
{
  if (destination->closed || !destination->writable) {
    return CALL_BAD_HANDLE;
  }
  if (destination->output_count || destination->has_pushback ||
      destination->has_wide_pushback) {
    return CALL_BAD_OPERATION;
  }
  struct pyxis_stdio_binding binding;
  if (descriptor_object_stream(destination->operation, &binding) < 0) {
    return CALL_BAD_HANDLE;
  }
  if (binding.protocol == PROTOCOL_PIPE) {
    *target = (struct handoff_target){binding.handle, true};
    return CALL_OK;
  }
  struct output_status_reply state;
  enum call_status status = output_producer_status(binding.handle, &state);
  if (status != CALL_OK) {
    return status;
  }
  *target = (struct handoff_target){binding.handle, false};
  return CALL_OK;
}

/* A pipe takes only the value; any observation stays with the caller. Every
 * pipe refusal before the move leaves the value with the caller, so each one
 * permits byte copying. */
static enum call_status handoff_to(const struct handoff_target *target,
    struct output_handoff_request *owned)
{
  if (!target->pipe) {
    return output_handoff(target->handle, owned);
  }
  if (!owned->value) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = pipe_handoff(target->handle, &owned->value);
  return status == CALL_WRONG_TYPE || status == CALL_DENIED ? CALL_BAD_OPERATION : status;
}

enum call_status pyxis_stdio_handoff_owned(FILE *destination,
    struct output_handoff_request *owned)
{
  if (!destination || !owned) {
    return CALL_BAD_REQUEST;
  }
  if (!stream_acquire(destination)) {
    return CALL_BAD_HANDLE;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {destination};
  struct handoff_target target;
  enum call_status status = handoff_destination(destination, &target);
  return status == CALL_OK ? handoff_to(&target, owned) : status;
}

enum call_status pyxis_stdio_handoff(FILE *destination, FILE *source)
{
  if (!destination || !source || source == destination) {
    return CALL_BAD_REQUEST;
  }
  if (!stream_acquire(destination)) {
    return CALL_BAD_HANDLE;
  }
  struct handoff_target target;
  enum call_status status = handoff_destination(destination, &target);
  stream_end(destination);
  if (status != CALL_OK) {
    return status;
  }
  if (!stream_acquire(source)) {
    return CALL_BAD_HANDLE;
  }
  struct output_handoff_request owned = {0};
  status = CALL_BAD_OPERATION;
  if (!source->closed && source->readable && !source->io_started &&
      !source->has_pushback && !source->has_wide_pushback && !source->output_count) {
    status = descriptor_object_output_copy(source->operation, &owned);
  }
  if (status == CALL_OK) {
    /* Eligible source objects are read-only, so they cannot also occupy the
     * writable destination role in a reversed pair of handoff helpers. */
    status = pyxis_stdio_handoff_owned(destination, &owned);
    descriptor_file_end(source->operation);
  }
  stream_end(source);
  int saved = errno;
  if (owned.observation) {
    descriptor_release_handle(owned.observation);
  }
  if (owned.value) {
    descriptor_release_handle(owned.value);
  }
  errno = saved;
  return status;
}

int pyxis_input_take(FILE *stream, handle_t *value)
{
  if (!value) {
    errno = EINVAL;
    return -1;
  }
  *value = HANDLE_INVALID;
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (stream->closed || !stream->readable || stream->eof || stream->has_pushback ||
      stream->has_wide_pushback) {
    return 0;
  }
  return descriptor_object_take_value(stream->operation, value);
}

static int flush_output(FILE *stream)
{
  while (stream->output_count) {
    if (!stream_ready(stream, true)) {
      stream->output_flush_failed = true;
      return EOF;
    }
    size_t written;
    if (descriptor_object_write(stream->operation, stream->output, stream->output_count, &written) < 0) {
      stream->output_flush_failed = true;
      return stream_error(stream, errno);
    }
    stream->output_count -= written;
    memmove(stream->output, stream->output + written, stream->output_count);
  }
  stream->output_flush_failed = false;
  return 0;
}

int libc_stdio_flush_descriptor(int descriptor)
{
  struct descriptor_object *object = descriptor_hold(descriptor);
  if (!object || !descriptor_object_ready(object, true)) {
    descriptor_drop(object);
    return -1;
  }
  FILE *stream = NULL;
  libc_lock_acquire(&stream_guard);
  for (FILE *current = streams; current; current = current->next) {
    if (__atomic_load_n(&current->descriptor, __ATOMIC_ACQUIRE) == descriptor &&
        !current->closing && current->references != UINT32_MAX) {
      ++current->references;
      stream = current;
      break;
    }
  }
  libc_lock_release(&stream_guard);
  int result = 0;
  if (stream) {
    if (stream_acquire_retained(stream)) {
      if (stream->operation == object) {
        result = stream->output_count ? flush_output(stream) : 0;
      } else {
        errno = EBADF;
        result = -1;
      }
      stream_end(stream);
    } else {
      result = -1;
    }
  }
  int saved = errno;
  descriptor_drop(object);
  errno = saved;
  return result;
}

static int acquire_close_stream(int descriptor, struct descriptor_object *expected, FILE **result)
{
  *result = NULL;
  FILE *stream = NULL;
  int error = 0;
  libc_lock_acquire(&stream_guard);
  for (FILE *current = streams; current; current = current->next) {
    if (__atomic_load_n(&current->descriptor, __ATOMIC_ACQUIRE) != descriptor) {
      continue;
    }
    if (current->closing) {
      error = EBUSY;
    } else if (current->references == UINT32_MAX) {
      error = EOVERFLOW;
    } else {
      ++current->references;
      stream = current;
    }
    break;
  }
  libc_lock_release(&stream_guard);
  if (error) {
    errno = error;
    return -1;
  }
  if (!stream) {
    return 0;
  }
  if (!stream_acquire_retained(stream)) {
    return -1;
  }
  if (stream->closing || stream->operation != expected ||
      __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE) != descriptor) {
    error = stream->closing ? EBUSY : EBADF;
    stream_end(stream);
    errno = error;
    return -1;
  }
  *result = stream;
  return 0;
}

static int stage_close_stream(FILE *stream, struct descriptor_object *expected)
{
  int error = stream->output_count && flush_output(stream) < 0 ? errno : 0;
  if (stream->error && !error) {
    error = EIO;
  }
  if (error) {
    descriptor_object_abandon(expected, EIO);
    errno = error;
    return -1;
  }
  return 0;
}

int libc_stdio_stage_close_descriptor(int descriptor, struct descriptor_object *expected)
{
  FILE *stream;
  if (acquire_close_stream(descriptor, expected, &stream) < 0) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return stream ? stage_close_stream(stream, expected) : 0;
}

int libc_stdio_close_descriptor(int descriptor, struct descriptor_object *expected,
    uint64_t generation)
{
  FILE *stream;
  if (acquire_close_stream(descriptor, expected, &stream) < 0) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  int error = stream && stage_close_stream(stream, expected) < 0 ? errno : 0;
  /* Keep FILE exclusion until the checked close invalidates its association. */
  int result = descriptor_close_expected(descriptor, expected, generation);
  if (error) {
    errno = error;
    return -1;
  }
  return result;
}

int setvbuf(FILE *restrict stream, char *restrict buffer, int mode, size_t size)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream || stream->closed || __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE) < 0) {
    errno = EBADF;
    return -1;
  }
  struct pyxis_stdio_binding binding;
  if (descriptor_object_stream(stream->operation, &binding) < 0) {
    return -1;
  }
  if (stream->io_started || (mode != _IONBF && mode != _IOLBF && mode != _IOFBF)
      || (mode != _IONBF && buffer && !size)) {
    errno = EINVAL;
    return -1;
  }
  if (mode != _IONBF && !stream->writable) {
    errno = ENOTSUP;
    return -1;
  }
  bool owned = mode != _IONBF && !buffer;
  size_t capacity = mode == _IONBF ? 0 : (size ? size : BUFSIZ);
  unsigned char *output = mode == _IONBF ? NULL : (unsigned char *)buffer;
  if (owned && !(output = malloc(capacity))) {
    return -1;
  }
  if (stream->output_owned) {
    free(stream->output);
  }
  stream->output = output;
  stream->output_capacity = capacity;
  stream->output_owned = owned;
  stream->output_mode = mode;
  stream->input_unbuffered = mode == _IONBF;
  return 0;
}

void setbuf(FILE *restrict stream, char *restrict buffer)
{
  if (!stream_acquire(stream)) {
    return;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

static int flush_all(void)
{
  FILE **snapshot = NULL;
  size_t count = 0;
  for (;;) {
    libc_lock_acquire(&stream_guard);
    size_t needed = 0;
    for (FILE *current = streams; current; current = current->next) {
      ++needed;
    }
    if (needed <= count) {
      size_t index = 0;
      for (FILE *current = streams; current; current = current->next) {
        if (!current->closing && current->references != UINT32_MAX) {
          ++current->references;
          snapshot[index++] = current;
        }
      }
      count = index;
      libc_lock_release(&stream_guard);
      break;
    }
    libc_lock_release(&stream_guard);
    free(snapshot);
    if (needed > SIZE_MAX / sizeof(*snapshot) ||
        !(snapshot = malloc(needed * sizeof(*snapshot)))) {
      errno = ENOMEM;
      return EOF;
    }
    count = needed;
  }
  int error = 0;
  for (size_t index = 0; index < count; ++index) {
    FILE *current = snapshot[index];
    if (stream_acquire_retained(current)) {
      if (current->writable && flush_output(current) < 0 && !error) {
        error = errno;
      }
      stream_end(current);
    } else if (!error) {
      error = errno;
    }
  }
  free(snapshot);
  if (error) {
    errno = error;
  }
  return error ? EOF : 0;
}

int fflush(FILE *stream)
{
  if (stream && !stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream) {
    return flush_all();
  }
  if (stream->closed) {
    return stream_error(stream, EBADF);
  }
  bool transaction = descriptor_object_transaction(stream->operation);
  if (transaction && !descriptor_object_transaction_pending(stream->operation)) {
    return 0;
  }
  struct pyxis_stdio_binding binding;
  if (descriptor_object_stream(stream->operation, &binding) < 0) {
    return stream_error(stream, errno);
  }
  stream->io_started = true;
  if (flush_output(stream) < 0) {
    return EOF;
  }
  if (transaction && stream->error) {
    errno = EIO;
    return EOF;
  }
  if (descriptor_object_commit(stream->operation) < 0) {
    return stream_error(stream, errno);
  }
  if (transaction) {
    stream->writable = false;
    stream->eof = false;
    stream->wide_input = (mbstate_t){0};
  }
  discard_pushback(stream);
  if (binding.protocol == PROTOCOL_FILE) {
    stream->wide_input = (mbstate_t){0};
  }
  descriptor_object_discard_input(stream->operation);
  return 0;
}

static void dispose_stream(FILE *stream)
{
  stream->closed = true;
  if (stream->output_owned) {
    free(stream->output);
  }
  stream->output = NULL;
  stream->output_count = 0;
}

static int close_stream(FILE *stream, bool abandon)
{
  libc_lock_acquire(&stream_guard);
  FILE **link = &streams;
  while (*link && *link != stream) {
    link = &(*link)->next;
  }
  if (!*link || stream->closing) {
    libc_lock_release(&stream_guard);
    errno = EBADF;
    return EOF;
  }
  if (stream->references == UINT32_MAX) {
    libc_lock_release(&stream_guard);
    errno = EOVERFLOW;
    return EOF;
  }
  stream->closing = true;
  ++stream->references; /* Closing owner prevents storage reclamation. */
  libc_lock_release(&stream_guard);
  /* Already admitted users retain storage; queued users can observe closed.
   * Raw descriptor close never waits on this FILE operation lock. */
  int error = libc_mutex_lock(&stream->lock);
  if (error) {
    __builtin_trap();
  }
  int descriptor = __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
  stream->operation = descriptor >= 0 ? descriptor_hold(descriptor) : NULL;
  bool cursor = descriptor_file_begin(stream->operation);
  if (abandon && descriptor_object_transaction_pending(stream->operation)) {
    stream->output_count = 0;
    error = descriptor_object_abandon(stream->operation, ECANCELED) < 0 ? errno : 0;
    if (descriptor_close_stream_abandon(stream, stream->operation) < 0 && !error) {
      error = errno;
    }
  } else {
    error = flush_output(stream) < 0 ? errno : 0;
    if (descriptor_object_transaction(stream->operation) && stream->error) {
      if (!error) {
        error = EIO;
      }
      if (descriptor_object_transaction_pending(stream->operation)) {
        descriptor_object_abandon(stream->operation, error);
      }
    }
    if (descriptor_close_stream(stream, stream->operation) < 0 && !error) {
      error = errno;
    }
  }
  if (cursor) {
    descriptor_file_end(stream->operation);
  }
  descriptor_drop(stream->operation);
  stream->operation = NULL;
  dispose_stream(stream);
  libc_lock_acquire(&stream_guard);
  link = &streams;
  while (*link != stream) {
    link = &(*link)->next;
  }
  *link = stream->next;
  libc_lock_release(&stream_guard);
  stream_unlock(stream);
  stream_release(stream);
  if (error) {
    errno = error;
  }
  return error ? EOF : 0;
}

int fclose(FILE *stream)
{
  return close_stream(stream, false);
}

int pyxis_fclose_abandon(FILE *stream)
{
  return close_stream(stream, true);
}

int stdio_finish(void)
{
  /* Normal exit has joined workers and closed thread admission. Drain every
   * producer before closing any alias; no allocation is needed to walk FILEs. */
  int error = 0;
  for (FILE *stream = streams; stream; stream = stream->next) {
    if (stream_acquire(stream)) {
      if (stream->writable && flush_output(stream) < 0 && !error) {
        error = errno;
      }
      stream_end(stream);
    } else if (!error) {
      error = errno;
    }
  }
  for (FILE *stream = streams; stream; stream = stream->next) {
    if (stream_acquire(stream)) {
      if (descriptor_object_transaction_pending(stream->operation) && stream->error) {
        if (!error) {
          error = EIO;
        }
        descriptor_object_abandon(stream->operation, EIO);
      }
      stream_end(stream);
    } else if (!error) {
      error = errno;
    }
  }
  if (error) {
    descriptor_report_finalize_error();
  }
  if (descriptor_finish() < 0 && !error) {
    error = errno;
  }
  while (streams) {
    FILE *stream = streams;
    streams = stream->next;
    dispose_stream(stream);
    if (stream->allocated) {
      free(stream);
    }
  }
  if (error) {
    errno = error;
  }
  return error ? EOF : 0;
}

/* Buffered reads may fetch ahead of the request; exact reads never do. */
static size_t read_some(void *buffer, size_t capacity, FILE *stream, bool buffered)
{
  stream->io_started = true;
  if (flush_output(stream) < 0) {
    return 0;
  }
  size_t read;
  int result = buffered && !stream->input_unbuffered ?
      descriptor_object_read_buffered(stream->operation, buffer, capacity, &read) :
      descriptor_object_read(stream->operation, buffer, capacity, &read);
  if (result < 0) {
    stream_error(stream, errno);
    return 0;
  }
  if (!read) {
    stream->eof = true;
  }
  return read;
}

/* Deliver an ungetc byte first; it is FILE state, not descriptor input. */
static bool take_pushback(unsigned char *byte, FILE *stream)
{
  if (!stream->has_pushback) {
    return false;
  }
  *byte = stream->pushback;
  stream->has_pushback = false;
  return true;
}

size_t fread_some(void *restrict buffer, size_t capacity, FILE *restrict stream)
{
  if (!capacity) {
    return 0;
  }
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_ready(stream, false)) {
    return 0;
  }
  if (!select_orientation(stream, STREAM_BYTE_ORIENTED)) {
    return 0;
  }
  if (take_pushback(buffer, stream)) {
    return 1;
  }
  if (stream->eof) {
    return 0;
  }
  return read_some(buffer, capacity, stream, false);
}

size_t fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
  if (!size || !count) {
    return 0;
  }
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_ready(stream, false)) {
    return 0;
  }
  if (count > SIZE_MAX / size) {
    stream_error(stream, EOVERFLOW);
    return 0;
  }
  if (!select_orientation(stream, STREAM_BYTE_ORIENTED)) {
    return 0;
  }
  size_t bytes = size * count, total = 0;
  if (take_pushback(buffer, stream)) {
    total = 1;
  } else if (stream->eof) {
    return 0;
  }
  while (total < bytes) {
    size_t read = read_some((char *)buffer + total, bytes - total, stream, true);
    if (!read) {
      break;
    }
    total += read;
  }
  /* Bytes in an incomplete final element have still been consumed. */
  return total / size;
}

static size_t write_bytes(const void *buffer, size_t bytes, FILE *stream)
{
  /* ISO C requires a positioning call between reading and writing; like
   * read-ahead, an unread pushback byte is dropped by the write. */
  discard_pushback(stream);
  stream->wide_input = (mbstate_t){0};
  stream->io_started = true;
  descriptor_object_discard_input(stream->operation);
  size_t total = 0;
  // A failed old queue keeps ownership even after clearerr; drain it first.
  if (stream->output_flush_failed && flush_output(stream) < 0) {
    return 0;
  }
  while (total < bytes) {
    if (stream->output_mode != _IONBF) {
      if (stream->output_count == stream->output_capacity
          && flush_output(stream) < 0) {
        break;
      }
      size_t chunk = stream->output_capacity - stream->output_count;
      if (chunk > bytes - total) {
        chunk = bytes - total;
      }
      const unsigned char *input = (const unsigned char *)buffer + total;
      bool newline = false;
      if (stream->output_mode == _IOLBF) {
        const unsigned char *end = memchr(input, '\n', chunk);
        if (end) {
          chunk = (size_t)(end - input) + 1;
          newline = true;
        }
      }
      memcpy(stream->output + stream->output_count, input, chunk);
      stream->output_count += chunk;
      total += chunk;
      if ((newline || stream->output_count == stream->output_capacity)
          && flush_output(stream) < 0) {
        break;
      }
      continue;
    }
    size_t written;
    if (descriptor_object_write(stream->operation, (const char *)buffer + total,
                         bytes - total, &written) < 0) {
      stream_error(stream, errno);
      break;
    }
    total += written;
  }
  /* Buffered bytes are accepted when copied into FILE ownership, including
   * retained output after a flush failure. Unbuffered bytes count only confirmed
   * backend progress. As usual, a partial final element may be consumed. */
  return total;
}

size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
  if (!size || !count) {
    return 0;
  }
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_ready(stream, true)) {
    return 0;
  }
  if (count > SIZE_MAX / size) {
    stream_error(stream, EOVERFLOW);
    return 0;
  }
  if (!select_orientation(stream, STREAM_BYTE_ORIENTED)) {
    return 0;
  }
  return write_bytes(buffer, size * count, stream) / size;
}

int fgetc(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  unsigned char byte;
  return fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int getc(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return fgetc(stream);
}

int getchar(void)
{
  if (!stream_acquire(stdin)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stdin};
  return fgetc(stdin);
}

char *fgets(char *restrict buffer, int capacity, FILE *restrict stream)
{
  if (!stream_acquire(stream)) {
    return NULL;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (capacity <= 0) {
    stream_error(stream, EINVAL);
    return NULL;
  }
  if (!stream_byte_ready(stream, false)) {
    return NULL;
  }
  int length = 0;
  while (length < capacity - 1) {
    int byte = fgetc(stream);
    if (byte == EOF) {
      /* Decide from this call's outcome: a failed read leaves EOF clear, while
       * an error indicator left by an earlier call must not reject a line. */
      if (!length || !feof(stream)) {
        return NULL;
      }
      break;
    }
    buffer[length++] = byte;
    if (byte == '\n') {
      break;
    }
  }
  buffer[length] = '\0';
  return buffer;
}

#define GETLINE_INITIAL_CAPACITY 128
/* Room for a SSIZE_MAX-byte line and its terminator. */
#define GETLINE_MAX_CAPACITY ((size_t)SSIZE_MAX + 1)

static bool grow_line(char **line, size_t *capacity, size_t required)
{
  size_t grown = *capacity < GETLINE_INITIAL_CAPACITY ? GETLINE_INITIAL_CAPACITY : *capacity;
  while (grown < required) {
    grown = grown > GETLINE_MAX_CAPACITY / 2 ? GETLINE_MAX_CAPACITY : grown * 2;
  }
  char *resized = realloc(*line, grown);
  if (!resized) {
    return false;
  }
  *line = resized;
  *capacity = grown;
  return true;
}

ssize_t getline(char **restrict line, size_t *restrict capacity, FILE *restrict stream)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!line || !capacity) {
    return stream_error(stream, EINVAL);
  }
  if (!*line) {
    *capacity = 0;
  }
  if (!stream_ready(stream, false)) {
    return -1;
  }
  size_t length = 0;
  while (true) {
    int byte = fgetc(stream);
    if (byte == EOF) {
      /* A failed read leaves EOF clear; only real EOF ends a partial line. */
      if (!length || !feof(stream)) {
        return -1;
      }
      break;
    }
    /* The byte is already consumed; there is no pushback to return it. */
    if (length == SSIZE_MAX) {
      return stream_error(stream, EOVERFLOW);
    }
    if (length + 2 > *capacity && !grow_line(line, capacity, length + 2)) {
      return stream_error(stream, ENOMEM);
    }
    (*line)[length++] = byte;
    if (byte == '\n') {
      break;
    }
  }
  (*line)[length] = '\0';
  return length;
}

static bool pushback_ready(FILE *stream)
{
  if (!stream_ready(stream, false)) {
    return false;
  }
  struct pyxis_stdio_binding binding;
  if (descriptor_object_stream(stream->operation, &binding) < 0) {
    stream_error(stream, errno);
    return false;
  }
  if (binding.protocol == PROTOCOL_STREAM) {
    stream_error(stream, ESPIPE);
    return false;
  }
  return true;
}

int ungetc(int character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (character == EOF || !pushback_ready(stream) || stream->has_pushback) {
    return EOF;
  }
  if (!select_orientation(stream, STREAM_BYTE_ORIENTED)) {
    return EOF;
  }
  stream->io_started = true;
  stream->pushback = (unsigned char)character;
  stream->has_pushback = true;
  stream->eof = false;
  return stream->pushback;
}

int fwide(FILE *stream, int mode)
{
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream || stream->closed) {
    errno = EBADF;
    return 0;
  }
  struct pyxis_stdio_binding binding;
  if (descriptor_object_stream(stream->operation, &binding) < 0) {
    return 0;
  }
  if (!stream->orientation && mode) {
    stream->orientation = mode > 0 ? STREAM_WIDE_ORIENTED : STREAM_BYTE_ORIENTED;
  }
  return stream->orientation;
}

wint_t fgetwc(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return WEOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_ready(stream, false) || !select_orientation(stream, STREAM_WIDE_ORIENTED)) {
    return WEOF;
  }
  stream->io_started = true;
  if (stream->has_wide_pushback) {
    stream->has_wide_pushback = false;
    return stream->wide_pushback;
  }
  if (stream->eof) {
    return WEOF;
  }
  for (;;) {
    char byte;
    if (!read_some(&byte, 1, stream, true)) {
      if (stream->eof && !mbsinit(&stream->wide_input)) {
        stream->wide_input = (mbstate_t){0};
        stream_error(stream, EILSEQ);
      }
      return WEOF;
    }
    wchar_t value;
    size_t count = mbrtowc(&value, &byte, 1, &stream->wide_input);
    if (count == (size_t)-1) {
      stream_error(stream, errno);
      return WEOF;
    }
    if (count != (size_t)-2) {
      return (wint_t)value;
    }
  }
}

wint_t getwc(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return WEOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return fgetwc(stream);
}

wint_t ungetwc(wint_t character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return WEOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (character == WEOF || !pushback_ready(stream) || stream->has_wide_pushback) {
    return WEOF;
  }
  if (!select_orientation(stream, STREAM_WIDE_ORIENTED)) {
    return WEOF;
  }
  char bytes[MB_LEN_MAX];
  mbstate_t state = {0};
  size_t length = wcrtomb(bytes, (wchar_t)character, &state);
  if (length == (size_t)-1) {
    return WEOF;
  }
  stream->io_started = true;
  stream->wide_pushback = character;
  stream->wide_pushback_bytes = length;
  stream->has_wide_pushback = true;
  stream->eof = false;
  return character;
}

wint_t fputwc(wchar_t character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return WEOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_ready(stream, true) || !select_orientation(stream, STREAM_WIDE_ORIENTED)) {
    return WEOF;
  }
  char bytes[MB_LEN_MAX];
  mbstate_t state = {0};
  size_t length = wcrtomb(bytes, character, &state);
  if (length == (size_t)-1) {
    stream_error(stream, errno);
    return WEOF;
  }
  size_t written = write_bytes(bytes, length, stream);
  return written == length && !stream->output_flush_failed ? (wint_t)character : WEOF;
}

wint_t putwc(wchar_t character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return WEOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return fputwc(character, stream);
}

int fputc(int character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  unsigned char byte = character;
  return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int putc(int character, FILE *stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return fputc(character, stream);
}

int putchar(int character)
{
  if (!stream_acquire(stdout)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stdout};
  return fputc(character, stdout);
}

int fputs(const char *restrict text, FILE *restrict stream)
{
  if (!stream_acquire(stream)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream_byte_ready(stream, true)) {
    return EOF;
  }
  size_t length = strlen(text);
  return fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

int puts(const char *text)
{
  if (!stream_acquire(stdout)) {
    return EOF;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stdout};
  return fputs(text, stdout) == EOF ? EOF : fputc('\n', stdout);
}

int feof(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return stream->eof;
}

int ferror(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return 0;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return stream->error;
}

void clearerr(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  stream->eof = false;
  stream->error = false;
}

int fseek(FILE *stream, long offset, int origin)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream || stream->closed) {
    errno = EBADF;
    return -1;
  }
  /* A relative seek counts from the position before any ungetc byte. */
  size_t pushed = pushback_bytes(stream);
  if (origin == SEEK_CUR && pushed) {
    if (offset < LONG_MIN + (long)pushed) {
      errno = EOVERFLOW;
      return -1;
    }
    offset -= (long)pushed;
  }
  if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
    errno = EINVAL;
    return -1;
  }
  stream->io_started = true;
  if (flush_output(stream) < 0 || descriptor_object_seek(stream->operation, offset, origin) < 0) {
    return -1;
  }
  stream->eof = false;
  discard_pushback(stream);
  stream->wide_input = (mbstate_t){0};
  return 0;
}

long ftell(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream || stream->closed) {
    errno = EBADF;
    return -1;
  }
  stream->io_started = true;
  long position = descriptor_object_tell_output(stream->operation, stream->output_count);
  /* The pushed-back byte is unread again. Before the first byte the position
   * stays zero; ISO C leaves it indeterminate there. */
  size_t pushed = pushback_bytes(stream);
  if (position > 0) {
    position = (uint64_t)position >= pushed ? position - (long)pushed : 0;
  }
  return position;
}

void rewind(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  fseek(stream, 0, SEEK_SET);
  clearerr(stream);
}

static_assert(sizeof(off_t) == sizeof(long), "fseeko and ftello forward to fseek and ftell");

int fseeko(FILE *stream, off_t offset, int origin)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return fseek(stream, offset, origin);
}

off_t ftello(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  return ftell(stream);
}

int fileno(FILE *stream)
{
  if (!stream_acquire(stream)) {
    return -1;
  }
  struct stream_operation operation __attribute__((cleanup(stream_operation_end))) = {stream};
  if (!stream || stream->closed || __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE) < 0) {
    errno = EBADF;
    return -1;
  }
  return __atomic_load_n(&stream->descriptor, __ATOMIC_ACQUIRE);
}
