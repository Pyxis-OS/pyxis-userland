/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#if !defined(__x86_64__)
#error "cpuclock dependent ADD chain currently requires x86-64"
#endif

#define MAX_WORKERS 64u
#define MAX_SAMPLES 1000u
#define MAX_MILLISECONDS 60000u
#define NS_PER_SECOND UINT64_C(1000000000)
#define ADDS_PER_UNROLL 256
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
#define UNROLLS_PER_BATCH 4096u
#define ADDS_PER_BATCH ((uint64_t)ADDS_PER_UNROLL * UNROLLS_PER_BATCH)
#define ADD_STEP UINT64_C(0x9e3779b97f4a7c15)

struct options {
  unsigned workers, samples, milliseconds;
};

struct barrier {
  mtx_t mutex;
  cnd_t changed;
  unsigned parties, arrived, generation;
  atomic_bool aborted;
  uint64_t duration, release, deadline;
  int clock_error;
};

struct sample {
  uint64_t adds, elapsed, skew, checksum;
  bool late, valid;
};

struct worker {
  struct barrier *barrier;
  struct sample *results;
  unsigned index, samples;
  int clock_error;
};

static void synchronization(int status)
{
  if (status != thrd_success) {
    fputs("cpuclock: thread synchronization failed\n", stderr);
    exit(EXIT_FAILURE);
  }
}

static bool monotonic(uint64_t *value)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return false;
  }
  if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= (long)NS_PER_SECOND ||
      (uint64_t)now.tv_sec > (UINT64_MAX - (uint64_t)now.tv_nsec) / NS_PER_SECOND) {
    errno = EOVERFLOW;
    return false;
  }
  *value = (uint64_t)now.tv_sec * NS_PER_SECOND + (uint64_t)now.tv_nsec;
  return true;
}

static void abort_workers(struct barrier *barrier)
{
  synchronization(mtx_lock(&barrier->mutex));
  atomic_store_explicit(&barrier->aborted, true, memory_order_release);
  synchronization(cnd_broadcast(&barrier->changed));
  synchronization(mtx_unlock(&barrier->mutex));
}

/* The final arrival opens each timed cohort. The second barrier only joins it.
 * Every generation is a predicate; wakeups alone never release a participant. */
static bool rendezvous(struct barrier *barrier, bool timed, uint64_t *release,
    uint64_t *deadline)
{
  synchronization(mtx_lock(&barrier->mutex));
  unsigned generation = barrier->generation;
  if (!atomic_load_explicit(&barrier->aborted, memory_order_acquire)) {
    if (++barrier->arrived == barrier->parties) {
      if (timed) {
        uint64_t start;
        if (!monotonic(&start)) {
          barrier->clock_error = errno;
          atomic_store_explicit(&barrier->aborted, true, memory_order_release);
        } else if (start > UINT64_MAX - barrier->duration) {
          barrier->clock_error = EOVERFLOW;
          atomic_store_explicit(&barrier->aborted, true, memory_order_release);
        } else {
          barrier->release = start;
          barrier->deadline = start + barrier->duration;
        }
      }
      barrier->arrived = 0;
      ++barrier->generation;
      synchronization(cnd_broadcast(&barrier->changed));
    } else {
      while (generation == barrier->generation &&
          !atomic_load_explicit(&barrier->aborted, memory_order_acquire)) {
        synchronization(cnd_wait(&barrier->changed, &barrier->mutex));
      }
    }
  }
  bool proceed = !atomic_load_explicit(&barrier->aborted, memory_order_acquire);
  if (release) {
    *release = barrier->release;
    *deadline = barrier->deadline;
  }
  synchronization(mtx_unlock(&barrier->mutex));
  return proceed;
}

__attribute__((noinline)) static uint64_t dependent_batch(uint64_t value)
{
  const uint64_t step = ADD_STEP;
  for (unsigned block = 0; block < UNROLLS_PER_BATCH; ++block) {
    /* Early clobber keeps STEP out of VALUE's register across all 256 adds. */
    __asm__ volatile(
        ".rept " STRINGIFY(ADDS_PER_UNROLL) "\n\t"
        "addq %[step], %[value]\n\t"
        ".endr\n\t"
        : [value] "+&r" (value)
        : [step] "r" (step)
        : "cc", "memory");
  }
  return value;
}

static int measure(void *argument)
{
  struct worker *worker = argument;
  for (unsigned number = 0; number < worker->samples; ++number) {
    uint64_t release, deadline;
    if (!rendezvous(worker->barrier, true, &release, &deadline)) {
      return 1;
    }
    struct sample *sample = &worker->results[number];
    uint64_t start;
    bool started = monotonic(&start);
    if (!started || start < release) {
      worker->clock_error = started ? EINVAL : errno;
      abort_workers(worker->barrier);
      return 1;
    }
    sample->skew = start - release;
    sample->late = start >= deadline;
    uint64_t seed = UINT64_C(0x123456789abcdef0) ^ worker->index ^
        ((uint64_t)number << 32);
    uint64_t value = seed, now = start;
    while (now < deadline) {
      value = dependent_batch(value);
      sample->adds += ADDS_PER_BATCH;
      uint64_t previous = now;
      bool timed = monotonic(&now);
      if (!timed || now < previous) {
        worker->clock_error = timed ? EINVAL : errno;
        abort_workers(worker->barrier);
        return 1;
      }
      if (atomic_load_explicit(&worker->barrier->aborted, memory_order_acquire)) {
        return 1;
      }
    }
    sample->elapsed = now - start;
    sample->checksum = value;
    sample->valid = !sample->late && sample->elapsed &&
        value == seed + sample->adds * ADD_STEP;
    if (!rendezvous(worker->barrier, false, NULL, NULL)) {
      return 1;
    }
  }
  return 0;
}

static bool number(const char *text, unsigned maximum, unsigned *value)
{
  unsigned result = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    unsigned digit = (unsigned)(*text - '0');
    if (result > (maximum - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  if (!result) {
    return false;
  }
  *value = result;
  return true;
}

static bool options(int argc, char **argv, struct options *result)
{
  *result = (struct options){.workers = 1, .samples = 5, .milliseconds = 1000};
  bool workers = false, samples = false, milliseconds = false;
  for (int i = 1; i < argc; ++i) {
    const char *name = argv[i];
    if (++i == argc) {
      return false;
    }
    if (!strcmp(name, "--threads") && !workers) {
      workers = true;
      if (!number(argv[i], MAX_WORKERS, &result->workers)) {
        return false;
      }
    } else if (!strcmp(name, "--samples") && !samples) {
      samples = true;
      if (!number(argv[i], MAX_SAMPLES, &result->samples)) {
        return false;
      }
    } else if (!strcmp(name, "--milliseconds") && !milliseconds) {
      milliseconds = true;
      if (!number(argv[i], MAX_MILLISECONDS, &result->milliseconds)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv)
{
  struct options selected;
  if (!options(argc, argv, &selected)) {
    fprintf(stderr, "usage: cpuclock [--threads 1..%u] [--samples 1..%u] "
        "[--milliseconds 1..%u]\n", MAX_WORKERS, MAX_SAMPLES, MAX_MILLISECONDS);
    return EXIT_FAILURE;
  }
  struct barrier barrier = {.parties = selected.workers,
      .duration = (uint64_t)selected.milliseconds * UINT64_C(1000000)};
  atomic_init(&barrier.aborted, false);
  synchronization(mtx_init(&barrier.mutex, mtx_plain));
  synchronization(cnd_init(&barrier.changed));
  struct worker *workers = calloc(selected.workers, sizeof(*workers));
  struct sample *results = calloc((size_t)selected.workers * selected.samples,
      sizeof(*results));
  thrd_t *threads = calloc(selected.workers, sizeof(*threads));
  if (!workers || !results || !threads) {
    fputs("cpuclock: measurement storage allocation failed\n", stderr);
    free(workers);
    free(results);
    free(threads);
    cnd_destroy(&barrier.changed);
    mtx_destroy(&barrier.mutex);
    return EXIT_FAILURE;
  }
  for (unsigned i = 0; i < selected.workers; ++i) {
    workers[i] = (struct worker){.barrier = &barrier,
        .results = results + (size_t)i * selected.samples, .index = i,
        .samples = selected.samples};
  }
  unsigned created = 0;
  bool failed = false, creation_failed = false;
  for (unsigned i = 1; i < selected.workers; ++i) {
    int status = thrd_create(&threads[i], measure, &workers[i]);
    if (status != thrd_success) {
      fprintf(stderr, "cpuclock: could not create worker %u of %u (C11 status %d)\n",
          i, selected.workers, status);
      abort_workers(&barrier);
      failed = true;
      creation_failed = true;
      break;
    }
    ++created;
  }
  int main_status = failed ? 1 : measure(&workers[0]);
  bool joined = true;
  for (unsigned i = 1; i <= created; ++i) {
    int status;
    if (thrd_join(threads[i], &status) != thrd_success) {
      joined = false;
      fputs("cpuclock: worker retirement unconfirmed\n", stderr);
    } else if (status) {
      failed = true;
    }
  }
  /* A failed creation/observation may retain runtime ownership. Keep the shared
   * context until process exit instead of freeing it under an unseen worker. */
  if (creation_failed || !joined) {
    _Exit(EXIT_FAILURE);
  }
  if (failed || main_status) {
    for (unsigned i = 0; i < selected.workers; ++i) {
      if (workers[i].clock_error) {
        fprintf(stderr, "cpuclock: worker %u CLOCK_MONOTONIC failed (errno %d)\n",
            i, workers[i].clock_error);
      }
    }
    if (barrier.clock_error) {
      fprintf(stderr, "cpuclock: cohort CLOCK_MONOTONIC failed (errno %d)\n",
          barrier.clock_error);
    }
    return EXIT_FAILURE;
  }
  printf("cpuclock: threads=%u samples=%u requested_ms=%u batch_adds=%" PRIu64
      " clock=CLOCK_MONOTONIC placement=unpinned\n", selected.workers,
      selected.samples, selected.milliseconds, ADDS_PER_BATCH);
  puts("cpuclock: GHz assumes 1 dependent register ADD per cycle; wall time includes descheduling/loop/clock overhead");
  double minimum = 0, maximum = 0, sum = 0;
  unsigned valid = 0, invalid = 0;
  for (unsigned round = 0; round < selected.samples; ++round) {
    for (unsigned i = 0; i < selected.workers; ++i) {
      struct sample *sample = &workers[i].results[round];
      if (!sample->valid) {
        printf("sample=%u worker=%u INVALID reason=%s start_skew_ms=%.3f\n",
            round + 1, i, sample->late ? "late-start" : "checksum-or-time",
            (double)sample->skew / 1000000.0);
        ++invalid;
        continue;
      }
      double ghz = (double)sample->adds / (double)sample->elapsed;
      printf("sample=%u worker=%u GHz=%.3f adds=%" PRIu64
          " elapsed_ms=%.3f start_skew_ms=%.3f checksum=%016" PRIx64 "\n",
          round + 1, i, ghz, sample->adds, (double)sample->elapsed / 1000000.0,
          (double)sample->skew / 1000000.0, sample->checksum);
      if (!valid || ghz < minimum) {
        minimum = ghz;
      }
      if (!valid || ghz > maximum) {
        maximum = ghz;
      }
      ++valid;
      sum += ghz;
    }
  }
  printf("cpuclock: %s threads=%u samples=%u valid=%u invalid=%u min_GHz=%.3f "
      "mean_GHz=%.3f max_GHz=%.3f\n", invalid ? "FAIL" : "PASS", selected.workers,
      selected.samples, valid, invalid, minimum, valid ? sum / valid : 0, maximum);
  cnd_destroy(&barrier.changed);
  mtx_destroy(&barrier.mutex);
  free(threads);
  free(results);
  free(workers);
  bool output_failed = fflush(stdout) != 0 || ferror(stdout);
  return invalid || output_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
