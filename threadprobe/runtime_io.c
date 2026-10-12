#include <abi/file.h>
#include <errno.h>
#include <fcntl.h>
#include <handle.h>
#include <pipe.h>
#include <pthread.h>
#include <pyxis/descriptor.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <startup.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define RECORD_BYTES (FILE_WRITE_MAX_BYTES + 1024)
#define WRITER_ROUNDS 8

struct io_file {
  const char *path;
  int descriptor;
  FILE *stream;
  bool created;
};

struct record_writer {
  FILE *stream;
  unsigned int index;
  bool formatted;
};

struct pipe_reader {
  FILE *stream;
  int descriptor;
  _Atomic bool started;
  ssize_t count;
  int error;
  bool eof, failed;
  char byte;
};

/* The checkpoint is for the matching debug ELF's manual GDB inspection.
 * started and a scheduling delay alone do not prove native CALL admission. */
unsigned int threadprobe_io_phase;
pthread_t threadprobe_io_waiter;
struct pipe_reader *threadprobe_io_reader;

__attribute__((noinline)) void threadprobe_io_checkpoint(void)
{
  __asm__ volatile("" ::: "memory");
}

static int failure(const char *operation)
{
  fprintf(stderr, "threadprobe runtime-io: %s (errno %d)\n", operation, errno);
  return 1;
}

static int file_create(struct io_file *file, const char *path)
{
  *file = (struct io_file){.path = path, .descriptor = -1};
  file->descriptor = open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (file->descriptor < 0) {
    return failure("exclusive tmp creation");
  }
  file->created = true;
  file->stream = fdopen(file->descriptor, "w+");
  return file->stream ? 0 : failure("fdopen tmp file");
}

static int file_close(struct io_file *file)
{
  int failed = 0;
  if (file->stream) {
    FILE *stream = file->stream;
    file->stream = NULL;
    file->descriptor = -1;
    if (fclose(stream)) {
      failed = failure("fclose tmp file");
    }
  } else if (file->descriptor >= 0) {
    int descriptor = file->descriptor;
    file->descriptor = -1;
    if (close(descriptor)) {
      failed = failure("close tmp descriptor");
    }
  }
  if (file->created) {
    file->created = false;
    if (unlink(file->path)) {
      failed = failure("remove owned tmp name");
    }
  }
  return failed;
}

static int write_records(void *argument)
{
  struct record_writer *writer = argument;
  unsigned char *record = malloc(RECORD_BYTES + 1);
  if (!record) {
    return 1;
  }
  unsigned char owner = 'A' + writer->index;
  memset(record, owner, RECORD_BYTES);
  record[0] = owner;
  record[2] = ':';
  record[RECORD_BYTES - 1] = '\n';
  record[RECORD_BYTES] = 0;
  int failed = 0;
  for (unsigned int round = 0; round < WRITER_ROUNDS; ++round) {
    record[1] = '0' + round;
    bool written = writer->formatted ?
        fprintf(writer->stream, "%s", (const char *)record) == RECORD_BYTES :
        fwrite(record, 1, RECORD_BYTES, writer->stream) == RECORD_BYTES;
    if (!written) {
      failed = 1;
      break;
    }
  }
  free(record);
  return failed;
}

static int records(bool aliases)
{
  struct io_file file;
  FILE *alias = NULL;
  int failed = file_create(&file, aliases ? "tmp://threadprobe-io-alias" :
      "tmp://threadprobe-io-shared");
  if (failed) {
    goto done;
  }
  if (aliases) {
    int descriptor = dup(file.descriptor);
    if (descriptor < 0) {
      failed = failure("dup record file");
      goto done;
    }
    alias = fdopen(descriptor, "w+");
    if (!alias) {
      close(descriptor);
      failed = failure("fdopen record alias");
      goto done;
    }
  }
  struct record_writer writers[2] = {
    {file.stream, 0, !aliases}, {alias ? alias : file.stream, 1, !aliases},
  };
  thrd_t threads[2];
  size_t created = 0;
  for (; created < 2; ++created) {
    int status = thrd_create(&threads[created], write_records, &writers[created]);
    if (status != thrd_success) {
      fprintf(stderr, "threadprobe runtime-io: writer CREATE %d\n", status);
      failed = 1;
      break;
    }
  }
  for (size_t index = 0; index < created; ++index) {
    int result = 1;
    int status = thrd_join(threads[index], &result);
    if (status != thrd_success) {
      fprintf(stderr, "threadprobe runtime-io: writer JOIN %d; lifetime unknown\n", status);
      _Exit(EXIT_FAILURE);
    }
    if (result) {
      failed = failure("record writer result");
    }
  }
  if (failed || fflush(file.stream) || (alias && fflush(alias)) ||
      fseek(file.stream, 0, SEEK_SET)) {
    if (!failed) {
      failed = failure("record rewind");
    }
    goto done;
  }
  unsigned char *record = malloc(RECORD_BYTES);
  if (!record) {
    failed = failure("record inspection allocation");
    goto done;
  }
  unsigned int seen[2] = {0};
  for (unsigned int index = 0; index < 2 * WRITER_ROUNDS && !failed; ++index) {
    if (fread(record, 1, RECORD_BYTES, file.stream) != RECORD_BYTES ||
        (record[0] != 'A' && record[0] != 'B') ||
        record[1] < '0' || record[1] >= '0' + WRITER_ROUNDS) {
      failed = failure("record framing");
      break;
    }
    unsigned int owner = record[0] - 'A';
    unsigned int round = record[1] - '0';
    unsigned int bit = 1u << round;
    if (seen[owner] & bit) {
      failed = failure("duplicate record");
      break;
    }
    seen[owner] |= bit;
    for (size_t byte = 2; byte < RECORD_BYTES; ++byte) {
      unsigned char expected = byte == 2 ? ':' :
          byte == RECORD_BYTES - 1 ? '\n' : 'A' + owner;
      if (record[byte] != expected) {
        failed = failure("interleaved record contents");
        break;
      }
    }
  }
  unsigned int all_rounds = (1u << WRITER_ROUNDS) - 1;
  if (!failed && (seen[0] != all_rounds || seen[1] != all_rounds ||
      fgetc(file.stream) != EOF || ferror(file.stream))) {
    failed = failure("record count/EOF");
  }
  free(record);
  if (!failed) {
    printf("threadprobe runtime-io: %s records=%u bytes=%zu PASS\n",
        aliases ? "dup-alias FILE fwrite" : "shared FILE fprintf",
        2 * WRITER_ROUNDS, (size_t)RECORD_BYTES);
  }
done:
  if (alias && fclose(alias)) {
    failed = failure("record alias close");
  }
  return file_close(&file) || failed;
}

static int positioned(void)
{
  struct io_file file;
  int failed = file_create(&file, "tmp://threadprobe-io-positioned");
  if (failed) {
    goto done;
  }
  char bytes[16];
  if (fwrite("abcdefghijklmnop", 1, 16, file.stream) != 16 ||
      fseek(file.stream, 0, SEEK_SET) || fread(bytes, 1, 3, file.stream) != 3 ||
      memcmp(bytes, "abc", 3) || lseek(file.descriptor, 0, SEEK_CUR) != 3 ||
      pread(file.descriptor, bytes, 3, 8) != 3 || memcmp(bytes, "ijk", 3) ||
      lseek(file.descriptor, 0, SEEK_CUR) != 3 ||
      pwrite(file.descriptor, "XYZ", 3, 3) != 3 ||
      lseek(file.descriptor, 0, SEEK_CUR) != 3 ||
      fread(bytes, 1, 3, file.stream) != 3 || memcmp(bytes, "XYZ", 3) ||
      ftell(file.stream) != 6 || fseek(file.stream, 0, SEEK_SET) ||
      fread(bytes, 1, 16, file.stream) != 16 || memcmp(bytes, "abcXYZghijklmnop", 16)) {
    failed = failure("positioned I/O/read-ahead coherence");
  }
  if (!failed) {
    puts("threadprobe runtime-io: positioned cursor/read-ahead PASS");
  }
done:
  return file_close(&file) || failed;
}

static int native_pipe(int *reader, int *writer)
{
  *reader = *writer = -1;
  struct pipe_create_reply ends;
  enum call_status status = pipe_create(startup_resource("pipe"), &ends);
  if (status != CALL_OK) {
    fprintf(stderr, "threadprobe runtime-io: pipe CREATE status %u\n", status);
    return 1;
  }
  *reader = pyxis_descriptor_adopt(&ends.reader, PYXIS_DESCRIPTOR_READ);
  if (*reader >= 0) {
    *writer = pyxis_descriptor_adopt(&ends.writer, PYXIS_DESCRIPTOR_WRITE);
  }
  if (*reader < 0 || *writer < 0) {
    if (*reader >= 0) {
      close(*reader);
      *reader = -1;
    }
    if (ends.reader != HANDLE_INVALID) {
      handle_close(ends.reader);
    }
    if (ends.writer != HANDLE_INVALID) {
      handle_close(ends.writer);
    }
    return failure("pipe descriptor adoption");
  }
  return 0;
}

static void *read_pipe(void *argument)
{
  struct pipe_reader *reader = argument;
  atomic_store_explicit(&reader->started, true, memory_order_release);
  errno = 0;
  reader->count = reader->stream ? (ssize_t)fread(&reader->byte, 1, 1, reader->stream) :
      read(reader->descriptor, &reader->byte, 1);
  reader->error = errno;
  if (reader->stream) {
    reader->eof = feof(reader->stream);
    reader->failed = ferror(reader->stream);
  }
  return NULL;
}

static int start_reader(struct pipe_reader *reader, unsigned int phase, bool *started)
{
  int error = pthread_create(&threadprobe_io_waiter, NULL, read_pipe, reader);
  if (error) {
    fprintf(stderr, "threadprobe runtime-io: reader CREATE %d\n", error);
    return 1;
  }
  *started = true;
  for (unsigned int attempt = 0; attempt < 1000 &&
      !atomic_load_explicit(&reader->started, memory_order_acquire); ++attempt) {
    struct timespec delay = {.tv_nsec = 1000000};
    if (thrd_sleep(&delay, NULL)) {
      return failure("reader scheduling delay");
    }
  }
  if (!atomic_load_explicit(&reader->started, memory_order_acquire)) {
    return failure("reader did not reach read entry");
  }
  struct timespec delay = {.tv_nsec = 20000000};
  if (thrd_sleep(&delay, NULL)) {
    return failure("reader inspection delay");
  }
  threadprobe_io_phase = phase;
  threadprobe_io_reader = reader;
  printf("threadprobe runtime-io: checkpoint phase=%u reader=%d thread=%llu\n",
      phase, reader->descriptor, (unsigned long long)threadprobe_io_waiter);
  threadprobe_io_checkpoint();
  return 0;
}

static int reader_join(void)
{
  int error = pthread_join(threadprobe_io_waiter, NULL);
  threadprobe_io_reader = NULL;
  threadprobe_io_waiter = 0;
  if (error) {
    fprintf(stderr, "threadprobe runtime-io: reader JOIN %d; lifetime unknown\n", error);
    _Exit(EXIT_FAILURE);
  }
  return 0;
}

static int active_file(void)
{
  struct io_file source;
  int reader = -1, writer = -1;
  FILE *stream = NULL;
  bool started = false;
  int failed = file_create(&source, "tmp://threadprobe-io-active");
  if (failed || native_pipe(&reader, &writer)) {
    failed = 1;
    goto done;
  }
  stream = fdopen(reader, "r");
  if (!stream) {
    failed = failure("fdopen active pipe");
    goto done;
  }
  struct pipe_reader context = {.stream = stream, .descriptor = reader};
  if (start_reader(&context, 1, &started)) {
    failed = 1;
    goto done;
  }
  errno = 0;
  if (dup2(source.descriptor, reader) != -1 || errno != EBUSY) {
    failed = failure("dup2 active FILE must refuse EBUSY");
  }
  if (close(writer)) {
    failed = failure("active pipe writer close");
  }
  writer = -1;
  if (reader_join()) {
    failed = 1;
  }
  started = false;
  if (context.count || !context.eof || context.failed) {
    fprintf(stderr, "threadprobe runtime-io: active read count=%ld errno=%d EOF=%u error=%u\n",
        (long)context.count, context.error, context.eof, context.failed);
    failed = 1;
  }
  if (!failed) {
    puts("threadprobe runtime-io: active FILE dup2 refusal PASS");
  }
done:
  if (writer >= 0) {
    if (close(writer)) {
      failed = failure("active writer cleanup");
    }
  }
  if (started && reader_join()) {
    failed = 1;
  }
  if (stream) {
    if (fclose(stream)) {
      failed = failure("active FILE cleanup");
    }
  } else if (reader >= 0 && close(reader)) {
    failed = failure("active reader cleanup");
  }
  return file_close(&source) || failed;
}

static int close_reuse(void)
{
  struct io_file replacement;
  int reader = -1, writer = -1, occupied = -1;
  bool started = false;
  int failed = file_create(&replacement, "tmp://threadprobe-io-reuse");
  if (failed) {
    goto done;
  }
  if (fwrite("replacement", 1, 11, replacement.stream) != 11) {
    failed = failure("prepare replacement bytes");
    goto done;
  }
  int closed = fclose(replacement.stream);
  replacement.stream = NULL;
  replacement.descriptor = -1;
  if (closed) {
    failed = failure("close prepared replacement");
    goto done;
  }
  if (native_pipe(&reader, &writer)) {
    failed = 1;
    goto done;
  }
  struct pipe_reader context = {.descriptor = reader};
  if (start_reader(&context, 2, &started)) {
    failed = 1;
    goto done;
  }
  int old_number = reader;
  if (close(reader)) {
    failed = failure("raw reader logical close");
  }
  reader = -1;
  occupied = open(replacement.path, O_RDONLY);
  if (occupied != old_number) {
    failed = failure("closed descriptor number reuse");
  }
  errno = 0;
  if (write(writer, "!", 1) != -1 || errno != EPIPE) {
    failed = failure("admitted read must not retain reader grant");
  }
  if (close(writer)) {
    failed = failure("final writer logical close");
  }
  writer = -1;
  if (reader_join()) {
    failed = 1;
  }
  started = false;
  char bytes[11];
  if (context.count || context.error) {
    fprintf(stderr, "threadprobe runtime-io: old admitted read count=%ld errno=%d\n",
        (long)context.count, context.error);
    failed = 1;
  }
  if (occupied < 0 || read(occupied, bytes, sizeof(bytes)) != sizeof(bytes) ||
      memcmp(bytes, "replacement", sizeof(bytes))) {
    failed = failure("replacement data/cursor untouched");
  }
  if (!failed) {
    puts("threadprobe runtime-io: raw CLOSE/reuse EOF/EPIPE PASS");
  }
done:
  if (writer >= 0 && close(writer)) {
    failed = failure("reuse writer cleanup");
  }
  if (started && reader_join()) {
    failed = 1;
  }
  if (reader >= 0 && close(reader)) {
    failed = failure("reuse reader cleanup");
  }
  if (occupied >= 0 && close(occupied)) {
    failed = failure("reoccupied descriptor cleanup");
  }
  return file_close(&replacement) || failed;
}

static int invalidated_file(void)
{
  struct io_file old, replacement = {.descriptor = -1};
  int failed = file_create(&old, "tmp://threadprobe-io-invalidated");
  if (failed) {
    goto done;
  }
  int old_number = old.descriptor;
  if (close(old.descriptor)) {
    failed = failure("invalidate FILE association");
    goto done;
  }
  old.descriptor = -1;
  if (file_create(&replacement, "tmp://threadprobe-io-occupant")) {
    failed = 1;
    goto done;
  }
  if (replacement.descriptor != old_number) {
    failed = failure("FILE invalidation number reuse");
  }
  errno = 0;
  int result = fclose(old.stream);
  old.stream = NULL;
  if (result != EOF || errno != EBADF ||
      fwrite("fresh", 1, 5, replacement.stream) != 5 ||
      fseek(replacement.stream, 0, SEEK_SET)) {
    failed = failure("fclose must preserve reoccupied descriptor");
  }
  char bytes[5];
  if (!failed && (fread(bytes, 1, sizeof(bytes), replacement.stream) != sizeof(bytes) ||
      memcmp(bytes, "fresh", sizeof(bytes)))) {
    failed = failure("reoccupied FILE contents");
  }
  if (!failed) {
    puts("threadprobe runtime-io: invalidated fclose preserves occupant PASS");
  }
done:
  int replacement_failed = file_close(&replacement);
  if (old.stream && old.descriptor < 0) {
    fclose(old.stream);
    old.stream = NULL;
  }
  return file_close(&old) || replacement_failed || failed;
}

static int buffered_replace(void)
{
  struct io_file source, target = {.descriptor = -1};
  int failed = file_create(&source, "tmp://threadprobe-io-source");
  if (failed || file_create(&target, "tmp://threadprobe-io-target")) {
    failed = 1;
    goto done;
  }
  char buffer[128];
  if (setvbuf(target.stream, buffer, _IOFBF, sizeof(buffer)) ||
      fwrite("queued", 1, 6, target.stream) != 6) {
    failed = failure("buffer target FILE output");
    goto done;
  }
  errno = 0;
  if (dup2(source.descriptor, target.descriptor) != -1 || errno != EBUSY ||
      fflush(target.stream) || dup2(source.descriptor, target.descriptor) != target.descriptor ||
      fwrite("source", 1, 6, source.stream) != 6 || fseek(target.stream, 0, SEEK_SET)) {
    failed = failure("buffered dup2 refusal/flush/replacement");
    goto done;
  }
  char bytes[6];
  if (fread(bytes, 1, sizeof(bytes), target.stream) != sizeof(bytes) ||
      memcmp(bytes, "source", sizeof(bytes))) {
    failed = failure("replacement shares source cursor/data");
    goto done;
  }
  FILE *original = fopen(target.path, "r");
  if (!original) {
    failed = failure("inspect detached target file");
    goto done;
  }
  if (fread(bytes, 1, sizeof(bytes), original) != sizeof(bytes) ||
      memcmp(bytes, "queued", sizeof(bytes))) {
    failed = failure("buffered refusal preserves old output");
  }
  if (fclose(original)) {
    failed = failure("original target inspection close");
  }
  if (!failed) {
    puts("threadprobe runtime-io: buffered dup2 refusal/replacement PASS");
  }
done:
  int target_failed = file_close(&target);
  return file_close(&source) || target_failed || failed;
}

int threadprobe_runtime_io(void)
{
  if (records(false) || records(true) || positioned() || buffered_replace() ||
      active_file() || close_reuse() || invalidated_file()) {
    return EXIT_FAILURE;
  }
  puts("threadprobe runtime-io: PASS (blocked CALL admission requires GDB checkpoints)");
  return EXIT_SUCCESS;
}
