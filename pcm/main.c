#include <audio.h>
#include <clock.h>
#include <startup.h>
#include <wait.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECONDS UINT64_C(3600)
#define MAX_PAUSE_MS UINT64_C(60000)
#define MAX_REPEAT UINT64_C(64)
#define DEFAULT_TAIL_MS UINT64_C(200)
#define LEFT_AMPLITUDE 8192
#define RIGHT_AMPLITUDE 4096
#define NS_PER_MS UINT64_C(1000000)
#define WAIT_INTERVAL_NS UINT64_C(1000000000)

struct options {
  uint64_t frames, pause_ms, gap_ms, tail_ms, repeat;
  uint64_t left_hz, right_hz;
};

struct producer {
  handle_t audio, clock;
  uint64_t left_phase, right_phase;
  uint64_t writes, full, waits;
};

static bool number(const char *text, uint64_t maximum, uint64_t *value)
{
  if (!*text) {
    return false;
  }
  uint64_t result = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    unsigned digit = *text - '0';
    if (result > maximum / 10 ||
        (result == maximum / 10 && digit > maximum % 10)) {
      return false;
    }
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
  *options = (struct options){.repeat = 1, .tail_ms = DEFAULT_TAIL_MS};
  bool explicit_frames = false;
  int index = 1;
  for (; index < argc && !strncmp(argv[index], "--", 2); index += 2) {
    if (index + 1 == argc) {
      return false;
    }
    uint64_t *destination, maximum;
    if (!strcmp(argv[index], "--frames")) {
      destination = &options->frames;
      maximum = AUDIO_RATE * MAX_SECONDS;
      explicit_frames = true;
    } else if (!strcmp(argv[index], "--pause-ms")) {
      destination = &options->pause_ms;
      maximum = MAX_PAUSE_MS;
    } else if (!strcmp(argv[index], "--gap-ms")) {
      destination = &options->gap_ms;
      maximum = MAX_PAUSE_MS;
    } else if (!strcmp(argv[index], "--tail-ms")) {
      destination = &options->tail_ms;
      maximum = MAX_PAUSE_MS;
    } else if (!strcmp(argv[index], "--repeat")) {
      destination = &options->repeat;
      maximum = MAX_REPEAT;
    } else {
      return false;
    }
    if (!number(argv[index + 1], maximum, destination)) {
      return false;
    }
  }
  if (argc - index != (explicit_frames ? 2 : 3) ||
      !number(argv[index], AUDIO_RATE / 2 - 1, &options->left_hz) ||
      !number(argv[index + 1], AUDIO_RATE / 2 - 1, &options->right_hz)) {
    return false;
  }
  if (!explicit_frames) {
    uint64_t seconds;
    if (!number(argv[index + 2], MAX_SECONDS, &seconds)) {
      return false;
    }
    options->frames = seconds * AUDIO_RATE;
  }
  return options->frames && options->repeat;
}

static int report_error(const char *operation, enum call_status status)
{
  fprintf(stderr, "pcm: %s (status %u)\n", operation, status);
  return EXIT_FAILURE;
}

static enum call_status print_status(handle_t audio, const char *when)
{
  struct audio_status_reply reply;
  enum call_status status = audio_status(audio, &reply);
  if (status == CALL_OK) {
    fprintf(stderr, "pcm: %s generation=%llu free=%llu/%llu starvation=%llu "
        "discontinuity=%llu state=%llu\n", when,
        (unsigned long long)reply.generation, (unsigned long long)reply.free_frames,
        (unsigned long long)reply.capacity_frames, (unsigned long long)reply.starvations,
        (unsigned long long)reply.discontinuities, (unsigned long long)reply.state);
  }
  return status;
}

static int16_t sample(uint64_t *phase, uint64_t frequency, int amplitude)
{
  if (!frequency) {
    return 0;
  }
  int value = *phase < AUDIO_RATE / 2 ?
      -amplitude + (int)(4 * amplitude * *phase / AUDIO_RATE) :
      3 * amplitude - (int)(4 * amplitude * *phase / AUDIO_RATE);
  *phase = (*phase + frequency) % AUDIO_RATE;
  return value;
}

static void store_sample(unsigned char *destination, int16_t value)
{
  uint16_t bits = (uint16_t)value;
  destination[0] = bits;
  destination[1] = bits >> 8;
}

static enum call_status write_frames(struct producer *producer,
    const unsigned char *bytes, size_t size)
{
  struct wait_interest interest = {producer->audio, WAIT_WRITABLE, 0};
  for (;;) {
    enum call_status status = audio_write(producer->audio, bytes, size);
    if (status != CALL_WOULD_BLOCK) {
      if (status == CALL_OK) {
        ++producer->writes;
      }
      return status;
    }
    ++producer->full;
    uint64_t now, events;
    status = clock_now(producer->clock, &now);
    if (status != CALL_OK) {
      return status;
    }
    if (now > UINT64_MAX - WAIT_INTERVAL_NS) {
      return CALL_LIMIT;
    }
    ++producer->waits;
    status = wait_many(&interest, 1, now + WAIT_INTERVAL_NS, &events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      return status;
    }
    if (events & WAIT_ERROR) {
      return CALL_UNAVAILABLE;
    }
  }
}

static enum call_status produce(struct producer *producer, const struct options *options)
{
  unsigned char bytes[AUDIO_WRITE_MAX];
  uint64_t position = 0, midpoint = options->frames / 2;
  bool paused = !options->pause_ms;
  while (position < options->frames) {
    if (!paused && position == midpoint) {
      enum call_status status = clock_sleep_for(producer->clock, options->pause_ms * NS_PER_MS);
      if (status != CALL_OK) {
        return status;
      }
      paused = true;
      status = print_status(producer->audio, "after pause");
      if (status != CALL_OK) {
        return status;
      }
    }
    uint64_t frames = options->frames - position;
    if (frames > AUDIO_WRITE_MAX / AUDIO_FRAME_BYTES) {
      frames = AUDIO_WRITE_MAX / AUDIO_FRAME_BYTES;
    }
    if (!paused && position + frames > midpoint) {
      frames = midpoint - position;
    }
    for (size_t i = 0; i < frames; ++i) {
      store_sample(bytes + i * AUDIO_FRAME_BYTES,
          sample(&producer->left_phase, options->left_hz, LEFT_AMPLITUDE));
      store_sample(bytes + i * AUDIO_FRAME_BYTES + 2,
          sample(&producer->right_phase, options->right_hz, RIGHT_AMPLITUDE));
    }
    enum call_status status = write_frames(producer, bytes, frames * AUDIO_FRAME_BYTES);
    if (status != CALL_OK) {
      return status;
    }
    position += frames;
  }
  return CALL_OK;
}

int main(int argc, char **argv)
{
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: pcm [--frames N] [--pause-ms N] [--repeat N] [--gap-ms N] "
        "[--tail-ms N] LEFT_HZ RIGHT_HZ [SECONDS]\n"
        "       zero Hz produces silence; --frames replaces SECONDS\n"
        "       pause occurs halfway; each repeat releases and reacquires\n", stderr);
    return EXIT_FAILURE;
  }
  struct producer producer = {
    .audio = startup_resource("audio"), .clock = startup_resource("clock"),
  };
  if (producer.audio == HANDLE_INVALID || producer.clock == HANDLE_INVALID) {
    fputs("pcm: requires audio playback and clock grants\n", stderr);
    return EXIT_FAILURE;
  }
  fprintf(stderr, "pcm: %llu frames/run, %llu runs, tail wait %llu ms "
      "(elapsed time does not guarantee audible drain)\n",
      (unsigned long long)options.frames, (unsigned long long)options.repeat,
      (unsigned long long)options.tail_ms);
  for (uint64_t run = 0; run < options.repeat; ++run) {
    struct audio_acquire_reply reply;
    enum call_status status = audio_acquire(producer.audio, &reply);
    if (status != CALL_OK) {
      return report_error("acquire", status);
    }
    uint64_t start = 0, finish = 0;
    if (reply.rate != AUDIO_RATE || reply.channels != AUDIO_CHANNELS ||
        reply.format != AUDIO_FORMAT_S16LE || reply.capacity_frames != AUDIO_QUEUE_FRAMES) {
      status = CALL_BAD_REQUEST;
    } else {
      status = print_status(producer.audio, "acquired");
    }
    if (status == CALL_OK) {
      status = clock_now(producer.clock, &start);
    }
    if (status == CALL_OK) {
      status = produce(&producer, &options);
    }
    if (status == CALL_OK) {
      status = clock_now(producer.clock, &finish);
    }
    if (status == CALL_OK) {
      fprintf(stderr, "pcm: run=%llu producer_ns=%llu writes=%llu full=%llu waits=%llu\n",
          (unsigned long long)(run + 1), (unsigned long long)(finish - start),
          (unsigned long long)producer.writes, (unsigned long long)producer.full,
          (unsigned long long)producer.waits);
      status = clock_sleep_for(producer.clock, options.tail_ms * NS_PER_MS);
    }
    if (status == CALL_OK) {
      status = print_status(producer.audio, "before release");
    }
    enum call_status released = audio_release(producer.audio);
    if (status != CALL_OK) {
      return report_error("produce", status);
    }
    if (released != CALL_OK) {
      return report_error("release", released);
    }
    if (run + 1 < options.repeat) {
      status = clock_sleep_for(producer.clock, options.gap_ms * NS_PER_MS);
      if (status != CALL_OK) {
        return report_error("gap", status);
      }
    }
  }
  return EXIT_SUCCESS;
}
