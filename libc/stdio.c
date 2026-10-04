#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "descriptor.h"
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
  *stream = (FILE){.descriptor = -1};
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
  *stream = (FILE){.descriptor = -1, .allocated = true};
  if (descriptor_open(path, &options, stream) < 0) {
    free(stream);
    return NULL;
  }
  register_stream(stream);
  return stream;
}

int fflush(FILE *stream)
{
  /* There is no output buffering. Input fflush is undefined in ISO C; Pyxis
   * drops file read-ahead so later reads refetch, and keeps pipe bytes. A prior
   * error indicator does not make the flush fail, and NULL leaves input alone. */
  if (stream && stream->closed) {
    return stream_error(stream, EBADF);
  }
  if (stream) {
    stream->has_pushback = false;
    descriptor_discard_input(stream->descriptor);
  }
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
  if (stream->allocated) {
    free(stream);
  }
}

int fclose(FILE *stream)
{
  if (!stream || stream->closed) {
    return stream_error(stream, EBADF);
  }
  int result = descriptor_close(stream->descriptor);
  if (result < 0) {
    stream_error(stream, errno);
  }
  dispose_stream(stream);
  return result < 0 ? EOF : 0;
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
  size_t read;
  int result = buffered ?
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
  size_t bytes = size * count, total = 0;
  while (total < bytes) {
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
  if (descriptor_seek(stream->descriptor, offset, origin) < 0) {
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
  long position = descriptor_tell(stream->descriptor);
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
