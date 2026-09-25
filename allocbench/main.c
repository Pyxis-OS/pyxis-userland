#include <clock.h>
#include <memory.h>
#include <profile.h>
#include <startup.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIVE_BYTES (64 * 1024 * 1024)
#define CLOCK_READS 1000

enum mode { HEAP, GROWTH, PAGES };

struct options {
  enum mode mode;
  size_t size, live, rounds;
  bool profile, mixed;
};

struct slot {
  void *pointer;
  size_t size;
};

struct results {
  uint64_t allocations, releases, failures;
  uint64_t live_bytes, peak_bytes;
  enum call_status error;
};

static bool number(const char *text, size_t maximum, size_t *result)
{
  size_t value = 0;
  if (!*text) {
    return false;
  }
  for (; *text; ++text) {
    if (*text < '0' || *text > '9' || value > (maximum - (*text - '0')) / 10) {
      return false;
    }
    value = value * 10 + (*text - '0');
  }
  *result = value;
  return value != 0;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
  if (argc < 2) {
    return false;
  }
  if (!strcmp(argv[1], "heap")) {
    *options = (struct options){HEAP, 64, 128, 4096, false, false};
  } else if (!strcmp(argv[1], "growth")) {
    *options = (struct options){GROWTH, 65536, 128, 1, false, false};
  } else if (!strcmp(argv[1], "pages")) {
    *options = (struct options){PAGES, 65536, 1, 64, false, false};
  } else {
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    const char *option = argv[i];
    if (!strcmp(option, "--profile") && !options->profile) {
      options->profile = true;
    } else if (!strcmp(option, "--mixed") && !options->mixed) {
      options->mixed = true;
    } else {
      if (++i == argc) {
        return false;
      }
      if (!strcmp(option, "--size")) {
        if (!number(argv[i], 1024 * 1024, &options->size)) {
          return false;
        }
      } else if (!strcmp(option, "--live")) {
        if (!number(argv[i], 65536, &options->live)) {
          return false;
        }
      } else if (!strcmp(option, "--rounds")) {
        if (!number(argv[i], 1000000, &options->rounds)) {
          return false;
        }
      } else {
        return false;
      }
    }
  }
  return options->size <= MAX_LIVE_BYTES / options->live &&
      (options->mode != GROWTH || options->rounds == 1) &&
      (options->mode != PAGES || (!options->mixed && !(options->size % MEMORY_PAGE_SIZE)));
}

static bool read_clock(handle_t clock, uint64_t *time)
{
  enum call_status status = clock_now(clock, time);
  if (status != CALL_OK) {
    fprintf(stderr, "allocbench: clock failed (status %u)\n", status);
    return false;
  }
  return true;
}

static bool measure_clock(handle_t clock, uint64_t *cost)
{
  uint64_t start, end;
  if (!read_clock(clock, &start)) {
    return false;
  }
  for (size_t i = 0; i < CLOCK_READS; ++i) {
    if (!read_clock(clock, &end)) {
      return false;
    }
  }
  *cost = (end - start) / CLOCK_READS;
  return true;
}

static size_t allocation_size(const struct options *options, size_t slot, size_t round)
{
  /* Eight deterministic sizes, rotated between rounds while other slots stay
   * live. This exercises reuse and holes without timing a random generator. */
  return options->mixed ? 1 + ((slot + round) % 8) * (options->size - 1) / 7 : options->size;
}

static bool allocate_slot(struct slot *slot, size_t size, enum mode mode,
    handle_t memory, struct results *results)
{
  ++results->allocations;
  if (mode == PAGES) {
    struct memory_region region;
    results->error = memory_allocate(memory, size, &region);
    if (results->error == CALL_OK) {
      slot->pointer = (void *)(uintptr_t)region.address;
      slot->size = region.size;
    }
  } else {
    slot->pointer = malloc(size);
    slot->size = size;
    results->error = slot->pointer ? CALL_OK : CALL_NO_MEMORY;
  }
  if (results->error != CALL_OK) {
    ++results->failures;
    return false;
  }
  /* Make use observable without turning this into a full-buffer bandwidth run. */
  volatile unsigned char *bytes = slot->pointer;
  bytes[0] = 0x5a;
  bytes[slot->size - 1] = 0xa5;
  results->live_bytes += slot->size;
  if (results->live_bytes > results->peak_bytes) {
    results->peak_bytes = results->live_bytes;
  }
  return true;
}

static bool release_slot(struct slot *slot, enum mode mode, handle_t memory,
    struct results *results)
{
  if (!slot->pointer) {
    return true;
  }
  ++results->releases;
  if (mode == PAGES) {
    struct memory_region region = {(uintptr_t)slot->pointer, slot->size};
    results->error = memory_release(memory, region);
    if (results->error != CALL_OK) {
      ++results->failures;
      return false;
    }
  } else {
    free(slot->pointer);
  }
  results->live_bytes -= slot->size;
  *slot = (struct slot){0};
  return true;
}

static bool run(const struct options *options, struct slot *slots, handle_t memory,
    struct results *results)
{
  for (size_t round = 0; round < options->rounds; ++round) {
    for (size_t i = 0; i < options->live; ++i) {
      if (options->mode == HEAP && !release_slot(&slots[i], HEAP, memory, results)) {
        return false;
      }
      if (!allocate_slot(&slots[i], allocation_size(options, i, round),
            options->mode, memory, results)) {
        return false;
      }
    }
    if (options->mode != HEAP) {
      /* Free alternating slots first, leaving holes among still-live blocks. */
      for (size_t parity = 0; parity < 2; ++parity) {
        for (size_t i = parity; i < options->live; i += 2) {
          if (!release_slot(&slots[i], options->mode, memory, results)) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

static void print_duration(const char *name, const struct profile_duration *duration,
    uint64_t count)
{
  printf("  %-11s total %.3f ms, mean %.3f us, max %.3f us\n", name,
      duration->total_ns / 1000000.0,
      count ? duration->total_ns / (double)count / 1000.0 : 0.0,
      duration->maximum_ns / 1000.0);
}

static void print_profile_operation(const char *name, const struct profile_memory_operation *stats)
{
  printf("%s: %llu requests, %llu failures, %llu requested / %llu completed bytes\n",
      name, (unsigned long long)stats->requests, (unsigned long long)stats->failures,
      (unsigned long long)stats->requested_bytes, (unsigned long long)stats->completed_bytes);
  if (stats->requests) {
    print_duration("publication", &stats->publication, stats->requests);
    print_duration("BSP queue", &stats->queue, stats->requests);
    print_duration("service", &stats->service, stats->requests);
    print_duration("resume", &stats->resume, stats->requests);
    print_duration("total", &stats->total, stats->requests);
  }
}

int main(int argc, char **argv)
{
  struct options options;
  if (!parse_options(argc, argv, &options)) {
    fputs("usage: allocbench heap|growth|pages [--profile] [--mixed]\n"
          "       [--size bytes] [--live count] [--rounds count]\n"
          "size: 1..1048576, live: 1..65536, rounds: 1..1000000, payload <=64 MiB\n"
          "growth uses one round; pages requires page-aligned size and no --mixed\n", stderr);
    return EXIT_FAILURE;
  }
  handle_t clock = startup_resource("clock"), memory = startup_resource("memory");
  handle_t profile = startup_resource("profile");
  if (clock == HANDLE_INVALID || memory == HANDLE_INVALID ||
      (options.profile && profile == HANDLE_INVALID)) {
    fputs("allocbench: missing clock, memory or requested profile grant\n", stderr);
    return EXIT_FAILURE;
  }
  struct slot *slots = calloc(options.live, sizeof(*slots));
  if (!slots) {
    fputs("allocbench: cannot allocate slot storage\n", stderr);
    return EXIT_FAILURE;
  }
  struct results results = {0};
  bool success = true;
  if (options.mode == HEAP) {
    for (size_t i = 0; i < options.live; ++i) {
      if (!allocate_slot(&slots[i], options.size, HEAP, memory, &results)) {
        success = false;
        break;
      }
    }
  }
  uint64_t clock_cost = 0;
  if (success) {
    success = measure_clock(clock, &clock_cost);
  }
  printf("allocbench %s: size=%zu live=%zu rounds=%zu mixed=%s profile=%s\n",
      argv[1], options.size, options.live, options.rounds,
      options.mixed ? "yes" : "no", options.profile ? "on" : "off");
  printf("Clock-call loop: %llu ns/read (%u reads); not subtracted\n",
      (unsigned long long)clock_cost, CLOCK_READS);
  fflush(stdout);

  results.allocations = results.releases = results.failures = 0;
  results.peak_bytes = results.live_bytes;
  bool profiling = false, measured = false;
  struct profile_snapshot snapshot = {0};
  uint64_t start = 0, end = 0;
  if (success && options.profile) {
    enum call_status status = profile_begin(profile);
    if (status != CALL_OK) {
      fprintf(stderr, "allocbench: profile begin failed (status %u)\n", status);
      success = false;
    } else {
      profiling = true;
    }
  }
  if (success && read_clock(clock, &start)) {
    success = run(&options, slots, memory, &results);
    measured = read_clock(clock, &end);
    success = success && measured;
  } else {
    success = false;
  }
  if (profiling) {
    enum call_status status = profile_end(profile, &snapshot);
    if (status != CALL_OK) {
      fprintf(stderr, "allocbench: profile end failed (status %u)\n", status);
      success = false;
      profiling = false;
    }
  }
  if (measured) {
    uint64_t elapsed = end - start;
    uint64_t operations = results.allocations + results.releases;
    printf("%llu allocation attempts, %llu releases, %llu failures; peak live payload %llu bytes\n",
        (unsigned long long)results.allocations, (unsigned long long)results.releases,
        (unsigned long long)results.failures, (unsigned long long)results.peak_bytes);
    printf("Elapsed %.3f ms; %.0f operations/s; %.1f ns/operation\n",
        elapsed / 1000000.0, elapsed ? operations * 1000000000.0 / elapsed : 0.0,
        operations ? elapsed / (double)operations : 0.0);
    if (profiling) {
      print_profile_operation("Backing allocate", &snapshot.allocate);
      print_profile_operation("Backing release", &snapshot.release);
      if (snapshot.flags & PROFILE_SATURATED) {
        fputs("Profile counters saturated; totals are lower bounds\n", stdout);
      }
    }
  }
  if (!success) {
    fprintf(stderr, "allocbench: incomplete run (last memory status %u)\n", results.error);
  }
  struct results cleanup = {.live_bytes = results.live_bytes};
  for (size_t i = 0; i < options.live; ++i) {
    if (!release_slot(&slots[i], options.mode, memory, &cleanup)) {
      success = false;
    }
  }
  free(slots);
  if (cleanup.failures) {
    fprintf(stderr, "allocbench: release failed; process exit will reclaim remaining backing\n");
  }
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
