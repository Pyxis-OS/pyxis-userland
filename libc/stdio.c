#include <abi/console.h>
#include <abi/file.h>
#include <abi/pipe.h>
#include <console.h>
#include <errno.h>
#include <file.h>
#include <handle.h>
#include <limits.h>
#include <pipe.h>
#include <startup.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "stream.h"

static FILE standard_input, standard_output, standard_error;
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;
static FILE *streams;

int stream_error(FILE *stream, int error)
{
  if (stream) {
    stream->error = true;
  }
  errno = error;
  return EOF;
}

bool stream_ready(FILE *stream, bool writing)
{
  if (!stream || stream->closed || stream->handle == HANDLE_INVALID) {
    stream_error(stream, stream && stream->open_error ? stream->open_error : EBADF);
    return false;
  }
  if (writing ? !stream->writable : !stream->readable) {
    stream_error(stream, EBADF);
    return false;
  }
  return true;
}

static void register_stream(FILE *stream)
{
  stream->next = streams;
  streams = stream;
}

static void standard_stream(FILE *stream, enum startup_stream_index index)
{
  struct startup_stream binding = startup_stream(index);
  *stream = (FILE){
    .kind = binding.protocol == PROTOCOL_FILE ? STREAM_FILE :
        binding.protocol == PROTOCOL_PIPE ? STREAM_PIPE : STREAM_CONSOLE,
    .handle = binding.handle,
    .readable = index == STARTUP_STDIN,
    .writable = index != STARTUP_STDIN,
    .open_error = binding.protocol == STARTUP_STREAM_NONE ? EBADF : 0,
  };
  register_stream(stream);
}

void stdio_init(void)
{
  /* Startup validated exclusive stream handles. Adopt each directly: retaining
   * another copy would keep a future pipe endpoint alive after fclose. */
  standard_stream(stdin, STARTUP_STDIN);
  standard_stream(stdout, STARTUP_STDOUT);
  standard_stream(stderr, STARTUP_STDERR);
}

static bool parse_mode(const char *mode, FILE *stream, bool *create, bool *truncate)
{
  if (!mode || (*mode != 'r' && *mode != 'w' && *mode != 'a')) {
    return false;
  }
  stream->readable = *mode == 'r';
  stream->writable = *mode != 'r';
  stream->append = *mode == 'a';
  *create = *mode != 'r';
  *truncate = *mode == 'w';
  bool binary = false, update = false;
  while (*++mode) {
    if (*mode == 'b' && !binary) {
      binary = true;
    } else if (*mode == '+' && !update) {
      update = true;
      stream->readable = true;
      stream->writable = true;
    } else {
      return false;
    }
  }
  return true;
}

FILE *fopen(const char *restrict path, const char *restrict mode)
{
  FILE initial = {.kind = STREAM_FILE, .allocated = true};
  bool create, truncate;
  if (!parse_mode(mode, &initial, &create, &truncate)) {
    errno = EINVAL;
    return NULL;
  }
  FILE *stream = malloc(sizeof(*stream));
  if (!stream) {
    return NULL;
  }
  *stream = initial;
  uint64_t rights = (stream->readable ? FILE_RIGHT_READ : 0) |
                    (stream->writable ? FILE_RIGHT_WRITE : 0);
  enum call_status status = stream_open_path(path, rights, create, &stream->handle);
  if (status == CALL_OK && truncate) {
    /* Allocate the FILE and resolve all authority before truncating. Nothing
     * fallible remains after a successful resize. */
    status = file_resize(stream->handle, 0);
  }
  if (status == CALL_OK && stream->append && !stream->readable) {
    status = file_size(stream->handle, &stream->position);
  }
  if (status != CALL_OK) {
    if (stream->handle != HANDLE_INVALID) {
      handle_close(stream->handle);
    }
    free(stream);
    errno = libc_call_errno(status);
    return NULL;
  }
  register_stream(stream);
  return stream;
}

int fflush(FILE *stream)
{
  /* There is neither output buffering nor input read-ahead to synchronize.
   * In particular, a prior error indicator does not make an empty flush fail. */
  if (stream && stream->closed) {
    return stream_error(stream, EBADF);
  }
  return 0;
}

int fclose(FILE *stream)
{
  if (!stream || stream->closed) {
    return stream_error(stream, EBADF);
  }
  int result = fflush(stream);
  if (stream->handle == HANDLE_INVALID || handle_close(stream->handle) != 0) {
    result = stream_error(stream, stream->open_error ? stream->open_error : EBADF);
  }
  FILE **link = &streams;
  while (*link && *link != stream) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = stream->next;
  }
  stream->closed = true;
  stream->handle = HANDLE_INVALID;
  if (stream->allocated) {
    free(stream);
  }
  return result;
}

void stdio_finish(void)
{
  fflush(NULL);
  while (streams) {
    fclose(streams);
  }
}

static size_t read_some(void *buffer, size_t capacity, FILE *stream)
{
  size_t read;
  enum call_status status;
  if (stream->kind == STREAM_FILE) {
    if (capacity > UINT64_MAX - stream->position) {
      stream_error(stream, EOVERFLOW);
      return 0;
    }
    status = file_read(stream->handle, stream->position, buffer, capacity, &read);
  } else if (stream->kind == STREAM_PIPE) {
    status = pipe_read(stream->handle, buffer, capacity, &read);
  } else {
    status = console_read(stream->handle, buffer, capacity, &read);
  }
  if (status != CALL_OK) {
    stream_error(stream, libc_call_errno(status));
    return 0;
  }
  if (!read) {
    if (stream->kind == STREAM_CONSOLE) {
      stream_error(stream, EIO); /* A nonempty terminal read has no EOF. */
    } else {
      stream->eof = true;
    }
    return 0;
  }
  if (stream->kind == STREAM_FILE) {
    stream->position += read;
  }
  return read;
}

size_t fread_some(void *restrict buffer, size_t capacity, FILE *restrict stream)
{
  if (!capacity || !stream_ready(stream, false) || stream->eof) {
    return 0;
  }
  return read_some(buffer, capacity, stream);
}

size_t fread(void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
  if (!size || !count) {
    return 0;
  }
  if (!stream_ready(stream, false)) {
    return 0;
  }
  if (count > SIZE_MAX / size) {
    stream_error(stream, EOVERFLOW);
    return 0;
  }
  if (stream->eof) {
    return 0;
  }
  size_t bytes = size * count, total = 0;
  while (total < bytes) {
    size_t read = read_some((char *)buffer + total, bytes - total, stream);
    if (!read) {
      break;
    }
    total += read;
  }
  /* Bytes in an incomplete final element have still been consumed. */
  return total / size;
}

size_t fwrite(const void *restrict buffer, size_t size, size_t count, FILE *restrict stream)
{
  if (!size || !count) {
    return 0;
  }
  if (!stream_ready(stream, true)) {
    return 0;
  }
  if (count > SIZE_MAX / size) {
    stream_error(stream, EOVERFLOW);
    return 0;
  }
  size_t bytes = size * count, total = 0;
  while (total < bytes) {
    size_t remaining = bytes - total;
    size_t written;
    uint64_t position = stream->position;
    enum call_status status;
    if (stream->kind == STREAM_FILE) {
      if (stream->append) {
        /* Deliberately non-atomic: another writer can change the end between
         * SIZE and WRITE. A native append operation remains future work. */
        status = file_size(stream->handle, &position);
        if (status != CALL_OK) {
          stream_error(stream, libc_call_errno(status));
          break;
        }
      }
      if (remaining > UINT64_MAX - position) {
        stream_error(stream, EOVERFLOW);
        break;
      }
      status = file_write(stream->handle, position, (const char *)buffer + total,
          remaining, &written);
    } else if (stream->kind == STREAM_PIPE) {
      status = pipe_write(stream->handle, (const char *)buffer + total, remaining, &written);
    } else {
      status = console_write(stream->handle, (const char *)buffer + total, remaining, &written);
    }
    if (status != CALL_OK || !written || written > remaining) {
      stream_error(stream, status == CALL_OK ? EIO : libc_call_errno(status));
      break;
    }
    if (stream->kind == STREAM_FILE) {
      stream->position = position + written;
    }
    total += written;
  }
  /* Report whole elements, but retain every confirmed byte in the position,
   * including a partial final element before a later failure. */
  return total / size;
}

int fgetc(FILE *stream)
{
  unsigned char byte;
  return fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int getc(FILE *stream)
{
  return fgetc(stream);
}

int getchar(void)
{
  return fgetc(stdin);
}

char *fgets(char *restrict buffer, int capacity, FILE *restrict stream)
{
  if (capacity <= 0) {
    stream_error(stream, EINVAL);
    return NULL;
  }
  if (!stream_ready(stream, false)) {
    return NULL;
  }
  int length = 0;
  while (length < capacity - 1) {
    int byte = fgetc(stream);
    if (byte == EOF) {
      if (ferror(stream) || !length) {
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

int fputc(int character, FILE *stream)
{
  unsigned char byte = character;
  return fwrite(&byte, 1, 1, stream) == 1 ? byte : EOF;
}

int putc(int character, FILE *stream)
{
  return fputc(character, stream);
}

int putchar(int character)
{
  return fputc(character, stdout);
}

int fputs(const char *restrict text, FILE *restrict stream)
{
  if (!stream_ready(stream, true)) {
    return EOF;
  }
  size_t length = strlen(text);
  return fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

int puts(const char *text)
{
  return fputs(text, stdout) == EOF ? EOF : fputc('\n', stdout);
}

int feof(FILE *stream)
{
  return stream->eof;
}

int ferror(FILE *stream)
{
  return stream->error;
}

void clearerr(FILE *stream)
{
  stream->eof = false;
  stream->error = false;
}

int fseek(FILE *stream, long offset, int origin)
{
  if (!stream || stream->closed || stream->handle == HANDLE_INVALID) {
    errno = EBADF;
    return -1;
  }
  if (stream->kind != STREAM_FILE) {
    errno = ESPIPE;
    return -1;
  }
  uint64_t base;
  if (origin == SEEK_SET) {
    base = 0;
  } else if (origin == SEEK_CUR) {
    base = stream->position;
  } else if (origin == SEEK_END) {
    enum call_status status = file_size(stream->handle, &base);
    if (status != CALL_OK) {
      errno = libc_call_errno(status);
      return -1;
    }
  } else {
    errno = EINVAL;
    return -1;
  }
  uint64_t distance = offset < 0 ? 0 - (uint64_t)offset : (uint64_t)offset;
  if ((offset < 0 && distance > base) || (offset >= 0 && distance > UINT64_MAX - base)) {
    errno = offset < 0 ? EINVAL : EOVERFLOW;
    return -1;
  }
  stream->position = offset < 0 ? base - distance : base + distance;
  stream->eof = false;
  return 0;
}

long ftell(FILE *stream)
{
  if (!stream || stream->closed || stream->handle == HANDLE_INVALID) {
    errno = EBADF;
  } else if (stream->kind != STREAM_FILE) {
    errno = ESPIPE;
  } else if (stream->position > LONG_MAX) {
    errno = EOVERFLOW;
  } else {
    return stream->position;
  }
  return -1;
}

void rewind(FILE *stream)
{
  fseek(stream, 0, SEEK_SET);
  clearerr(stream);
}
