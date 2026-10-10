#include <abi/directory.h>
#include <errno.h>
#include <handle.h>
#include <limits.h>
#include <stdint.h>
#include <startup.h>
#include <pyxis/stdio.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
#include "errors.h"
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
  if (!stream || stream->closed) {
    stream_error(stream, EBADF);
    return false;
  }
  if (writing ? !stream->writable : !stream->readable) {
    stream_error(stream, EBADF);
    return false;
  }
  if (!descriptor_ready(stream->descriptor, writing)) {
    stream_error(stream, errno);
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
  *stream = (FILE){
    .descriptor = -1, .readable = index == STARTUP_STDIN,
    .writable = index != STARTUP_STDIN,
  };
  descriptor_adopt_standard(index, stream);
  register_stream(stream);
}

void stdio_init(void)
{
  standard_stream(stdin, STARTUP_STDIN);
  standard_stream(stdout, STARTUP_STDOUT);
  standard_stream(stderr, STARTUP_STDERR);
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

FILE *fopen(const char *restrict path, const char *restrict mode)
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
    .descriptor = -1, .allocated = true,
    .readable = options.readable, .writable = options.writable,
  };
  if (descriptor_open(path, &options, stream) < 0) {
    free(stream);
    return NULL;
  }
  register_stream(stream);
  return stream;
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
    .descriptor = -1, .allocated = true,
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
    .descriptor = -1, .allocated = true, .readable = true, .writable = true,
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

int pyxis_stdio_stream(FILE *stream, struct startup_stream *binding)
{
  if (!binding) {
    errno = EINVAL;
    return -1;
  }
  *binding = (struct startup_stream){
    .protocol = STARTUP_STREAM_NONE, .handle = HANDLE_INVALID,
  };
  if (!stream) {
    errno = EINVAL;
    return -1;
  }
  bool standard = stream == &standard_input || stream == &standard_output ||
      stream == &standard_error;
  if (standard && stream->descriptor < 0) {
    return 0;
  }
  FILE *current = streams;
  while (current && current != stream) {
    current = current->next;
  }
  if (!current || current->closed) {
    errno = EBADF;
    return -1;
  }
  return descriptor_stream(current, binding);
}

static int flush_output(FILE *stream)
{
  while (stream->output_count) {
    if (!stream_ready(stream, true)) {
      return EOF;
    }
    size_t written;
    if (descriptor_write(stream->descriptor, stream->output, stream->output_count, &written) < 0) {
      return stream_error(stream, errno);
    }
    stream->output_count -= written;
    memmove(stream->output, stream->output + written, stream->output_count);
  }
  return 0;
}

int setvbuf(FILE *restrict stream, char *restrict buffer, int mode, size_t size)
{
  if (!stream || stream->closed || stream->descriptor < 0) {
    errno = EBADF;
    return -1;
  }
  struct startup_stream binding;
  if (descriptor_stream(stream, &binding) < 0) {
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
  setvbuf(stream, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

int fflush(FILE *stream)
{
  if (!stream) {
    int error = 0;
    for (FILE *current = streams; current; current = current->next) {
      if (current->writable && flush_output(current) < 0 && !error) {
        error = errno;
      }
    }
    if (error) {
      errno = error;
      return EOF;
    }
    return 0;
  }
  if (stream->closed) {
    return stream_error(stream, EBADF);
  }
  struct startup_stream binding;
  if (descriptor_stream(stream, &binding) < 0) {
    return stream_error(stream, errno);
  }
  stream->io_started = true;
  if (flush_output(stream) < 0) {
    return EOF;
  }
  stream->has_pushback = false;
  descriptor_discard_input(stream->descriptor);
  return 0;
}

static void dispose_stream(FILE *stream)
{
  FILE **link = &streams;
  while (*link && *link != stream) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = stream->next;
  }
  stream->closed = true;
  if (stream->output_owned) {
    free(stream->output);
  }
  stream->output = NULL;
  stream->output_count = 0;
  if (stream->allocated) {
    free(stream);
  }
}

int fclose(FILE *stream)
{
  if (!stream || stream->closed) {
    return stream_error(stream, EBADF);
  }
  int error = flush_output(stream) < 0 ? errno : 0;
  if (descriptor_close(stream->descriptor) < 0 && !error) {
    error = errno;
  }
  if (error) {
    stream_error(stream, error);
  }
  dispose_stream(stream);
  return error ? EOF : 0;
}

void stdio_finish(void)
{
  fflush(NULL);
  descriptor_finish();
  while (streams) {
    dispose_stream(streams);
  }
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
      descriptor_read_buffered(stream->descriptor, buffer, capacity, &read) :
      descriptor_read(stream->descriptor, buffer, capacity, &read);
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
  if (!capacity || !stream_ready(stream, false)) {
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
  if (!stream_ready(stream, false)) {
    return 0;
  }
  if (count > SIZE_MAX / size) {
    stream_error(stream, EOVERFLOW);
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

static int flush_write(FILE *stream, size_t *previous_pending)
{
  size_t before = stream->output_count;
  int result = flush_output(stream);
  size_t confirmed = before - stream->output_count;
  *previous_pending -= confirmed < *previous_pending ? confirmed : *previous_pending;
  return result;
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
  /* ISO C requires a positioning call between reading and writing; like
   * read-ahead, an unread pushback byte is dropped by the write. */
  stream->has_pushback = false;
  stream->io_started = true;
  descriptor_discard_input(stream->descriptor);
  size_t bytes = size * count, total = 0;
  size_t previous_pending = stream->output_count;
  bool output_error = false;
  while (total < bytes) {
    if (stream->output_mode != _IONBF) {
      if (stream->output_count == stream->output_capacity
          && flush_write(stream, &previous_pending) < 0) {
        output_error = true;
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
          && flush_write(stream, &previous_pending) < 0) {
        output_error = true;
        break;
      }
      continue;
    }
    size_t written;
    if (descriptor_write(stream->descriptor, (const char *)buffer + total,
                         bytes - total, &written) < 0) {
      stream_error(stream, errno);
      break;
    }
    total += written;
  }
  /* Report whole elements, but retain every confirmed byte in the position,
   * including a partial final element before a later failure. */
  if (output_error) {
    total -= stream->output_count - previous_pending;
  }
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

int ungetc(int character, FILE *stream)
{
  if (character == EOF || !stream_ready(stream, false) || stream->has_pushback) {
    return EOF;
  }
  stream->io_started = true;
  stream->pushback = (unsigned char)character;
  stream->has_pushback = true;
  stream->eof = false;
  return stream->pushback;
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
  if (!stream || stream->closed) {
    errno = EBADF;
    return -1;
  }
  /* A relative seek counts from the position before any ungetc byte. */
  if (origin == SEEK_CUR && stream->has_pushback) {
    if (offset == LONG_MIN) {
      errno = EOVERFLOW;
      return -1;
    }
    --offset;
  }
  if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) {
    errno = EINVAL;
    return -1;
  }
  stream->io_started = true;
  if (flush_output(stream) < 0 || descriptor_seek(stream->descriptor, offset, origin) < 0) {
    return -1;
  }
  stream->eof = false;
  stream->has_pushback = false;
  return 0;
}

long ftell(FILE *stream)
{
  if (!stream || stream->closed) {
    errno = EBADF;
    return -1;
  }
  stream->io_started = true;
  long position = descriptor_tell_output(stream->descriptor, stream->output_count);
  /* The pushed-back byte is unread again. Before the first byte the position
   * stays zero; ISO C leaves it indeterminate there. */
  if (position > 0 && stream->has_pushback) {
    --position;
  }
  return position;
}

void rewind(FILE *stream)
{
  fseek(stream, 0, SEEK_SET);
  clearerr(stream);
}

static_assert(sizeof(off_t) == sizeof(long), "fseeko and ftello forward to fseek and ftell");

int fseeko(FILE *stream, off_t offset, int origin)
{
  return fseek(stream, offset, origin);
}

off_t ftello(FILE *stream)
{
  return ftell(stream);
}

int fileno(FILE *stream)
{
  if (!stream || stream->closed || stream->descriptor < 0) {
    errno = EBADF;
    return -1;
  }
  return stream->descriptor;
}

int pyxis_stdio_response(FILE *stream, struct pyxis_response_info *info)
{
  if (!info) {
    errno = EINVAL;
    return -1;
  }
  *info = (struct pyxis_response_info){0};
  if (!stream) {
    errno = EINVAL;
    return -1;
  }
  FILE *current = streams;
  while (current && current != stream) {
    current = current->next;
  }
  if (!current || current->closed) {
    errno = EBADF;
    return -1;
  }
  if (stream->descriptor < 0) {
    enum startup_stream_index index = stream == &standard_input ? STARTUP_STDIN :
        stream == &standard_output ? STARTUP_STDOUT : STARTUP_STDERR;
    bool standard = stream == &standard_input || stream == &standard_output ||
        stream == &standard_error;
    if (standard && startup_stream(index).protocol == STARTUP_STREAM_NONE) {
      return 0;
    }
  }
  return descriptor_response(stream->descriptor, info);
}
