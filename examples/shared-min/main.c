#include <clock.h>
#include <directory.h>
#include <endpoint.h>
#include <environment.h>
#include <file.h>
#include <handle.h>
#include <memory.h>
#include <pyxis/job.h>
#include <pyxis/shared_memory.h>
#include <shared_memory.h>
#include <startup.h>
#include <wait.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_BYTES (64 * 1024)

struct shared_buffer {
  _Atomic uint64_t sequence;
  unsigned char data[BUFFER_BYTES];
};

struct counts {
  uint64_t sequence, bytes, lines;
};

struct next_span {
  uint64_t sequence, bytes, stop;
};

static void report_status(const char *operation, enum call_status status)
{
  fprintf(stderr, "shared-min: %s failed (status %u)%s\n", operation, (unsigned)status,
      status == CALL_OUTCOME_UNKNOWN ? "; outcome unknown, not replayed" : "");
}

static bool close_owned(handle_t *handle)
{
  handle_t owned = *handle;
  *handle = HANDLE_INVALID;
  if (owned != HANDLE_INVALID && handle_close(owned) != 0) {
    fputs("shared-min: handle close failed\n", stderr);
    return false;
  }
  return true;
}

static bool startup_capability(const char *name, handle_t *handle)
{
  enum call_status status = environment_capability(startup_environment_handle(), name, handle);
  if (status != CALL_OK) {
    report_status(name, status);
    return false;
  }
  return true;
}

static int worker(void)
{
  handle_t memory = HANDLE_INVALID, region = HANDLE_INVALID, caller = HANDLE_INVALID;
  struct memory_reservation view = {0};
  bool ok = startup_capability("memory", &memory) &&
      startup_capability("buffer", &region) && startup_capability("caller", &caller);
  if (ok) {
    enum call_status status = shared_memory_map(region, SHARED_MEMORY_RIGHT_READ, &view);
    if (status != CALL_OK) {
      report_status("worker MAP", status);
      ok = false;
    }
  }
  if (ok && view.size < sizeof(struct shared_buffer)) {
    fputs("shared-min: worker buffer is too small\n", stderr);
    ok = false;
  }
  struct counts counted = {0};
  const struct shared_buffer *buffer = (const void *)(uintptr_t)view.address;
  while (ok) {
    struct endpoint_packet reply;
    enum call_status status = endpoint_request(caller, &counted, sizeof(counted),
        NULL, 0, 0, &reply);
    if (status != CALL_OK) {
      report_status("worker CALL", status);
      ok = false;
      break;
    }
    struct next_span next;
    if (reply.result || reply.grant_count || reply.size != sizeof(next)) {
      fputs("shared-min: invalid worker reply\n", stderr);
      for (size_t i = 0; i < reply.grant_count; ++i) {
        close_owned(&reply.grants[i].handle);
      }
      ok = false;
      break;
    }
    memcpy(&next, reply.data, sizeof(next));
    if (next.stop == 1 && !next.bytes && next.sequence == counted.sequence) {
      break;
    }
    if (next.stop || !next.bytes || next.bytes > BUFFER_BYTES ||
        next.sequence != counted.sequence + 1) {
      fputs("shared-min: invalid shared span\n", stderr);
      ok = false;
      break;
    }
    if (atomic_load_explicit(&buffer->sequence, memory_order_acquire) != next.sequence) {
      fputs("shared-min: invalid publication\n", stderr);
      ok = false;
      break;
    }
    counted = (struct counts){.sequence = next.sequence, .bytes = next.bytes};
    for (size_t i = 0; i < next.bytes; ++i) {
      counted.lines += buffer->data[i] == '\n';
    }
    /* The next CALL acknowledges the span before the parent reuses it. */
  }
  if (view.identity) {
    enum call_status status = memory_release_reservation(memory, view.identity);
    if (status != CALL_OK) {
      report_status("worker unmap", status);
      ok = false;
    }
  }
  ok = close_owned(&caller) && ok;
  ok = close_owned(&region) && ok;
  ok = close_owned(&memory) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool receive_counts(handle_t receiver, handle_t clock,
    struct pyxis_job *job, struct endpoint_packet *request)
{
  struct wait_interest interests[] = {
    {.handle = receiver, .events = WAIT_READABLE},
    {.handle = pyxis_job_observer(job), .events = WAIT_COMPLETE},
  };
  for (;;) {
    uint64_t now, events[2];
    enum call_status status = clock_coarse_now(clock, &now);
    if (status != CALL_OK) {
      report_status("read clock", status);
      return false;
    }
    if (now > UINT64_MAX - WAIT_MAX_WAIT_NS) {
      fputs("shared-min: clock deadline overflow\n", stderr);
      return false;
    }
    status = wait_many(interests, 2, now + WAIT_MAX_WAIT_NS, events);
    if (status == CALL_TIMED_OUT) {
      continue;
    }
    if (status != CALL_OK) {
      report_status("wait for worker", status);
      return false;
    }
    if (events[0] & WAIT_READABLE) {
      status = endpoint_receive(receiver, request);
      if (status != CALL_OK) {
        report_status("receive worker counts", status);
      }
      return status == CALL_OK;
    }
    if ((events[0] & (WAIT_CLOSED | WAIT_ERROR)) || (events[1] & WAIT_COMPLETE)) {
      fputs("shared-min: worker completed or endpoint closed before counts\n", stderr);
      return false;
    }
  }
}

static bool transfer_input(handle_t receiver, handle_t clock, struct pyxis_job *job,
    struct shared_buffer *buffer, uintmax_t *bytes, uintmax_t *lines)
{
  uint64_t sequence = 0, previous_size = 0;
  for (;;) {
    struct endpoint_packet request;
    if (!receive_counts(receiver, clock, job, &request)) {
      return false;
    }
    handle_t receipt = request.receipt;
    struct counts counted;
    bool valid = request.kind == ENDPOINT_MESSAGE_CALL && !request.grant_count &&
        request.size == sizeof(counted);
    if (valid) {
      memcpy(&counted, request.data, sizeof(counted));
      valid = counted.sequence == sequence && counted.bytes == previous_size &&
          counted.lines <= counted.bytes && counted.bytes <= UINTMAX_MAX - *bytes &&
          counted.lines <= UINTMAX_MAX - *lines;
    }
    if (!valid) {
      fputs("shared-min: invalid or unrepresentable worker counts\n", stderr);
      for (size_t i = 0; i < request.grant_count; ++i) {
        close_owned(&request.grants[i].handle);
      }
      if (request.kind == ENDPOINT_MESSAGE_CALL || request.kind == ENDPOINT_MESSAGE_SEND) {
        close_owned(&receipt);
      }
      return false;
    }
    *bytes += counted.bytes;
    *lines += counted.lines;
    size_t size = fread(buffer->data, 1, sizeof(buffer->data), stdin);
    if (ferror(stdin)) {
      fputs("shared-min: stdin read failed\n", stderr);
      close_owned(&receipt);
      return false;
    }
    struct next_span next = {.sequence = sequence, .stop = !size};
    if (size) {
      next.sequence = ++sequence;
      next.bytes = size;
      previous_size = size;
      atomic_store_explicit(&buffer->sequence, sequence, memory_order_release);
    }
    enum call_status status = endpoint_reply(receipt, 0, &next, sizeof(next), NULL, 0);
    if (status != CALL_OK) {
      report_status("publish next span", status);
      close_owned(&receipt);
      return false;
    }
    if (!size) {
      return true;
    }
  }
}

static bool finish_worker(struct pyxis_job **job, bool cancel)
{
  if (!*job) {
    return true;
  }
  bool ok = true;
  if (cancel) {
    enum call_status status = pyxis_job_terminate(*job);
    if (status != CALL_OK) {
      report_status("terminate worker scope", status);
      ok = false;
    }
  }
  struct process_result result;
  enum call_status status = pyxis_job_wait(*job, &result);
  if (status != CALL_OK) {
    report_status("join worker scope", status);
    ok = false;
    if (!cancel) {
      status = pyxis_job_terminate(*job);
      if (status != CALL_OK) {
        report_status("terminate after failed join", status);
      }
      /* Observe cleanup after cancellation; never replay leaf admission. */
      status = pyxis_job_wait(*job, &result);
      if (status != CALL_OK) {
        report_status("join cancelled scope", status);
      }
    }
  } else if (result.kind != PROCESS_EXITED || result.exit_status) {
    fprintf(stderr, "shared-min: worker result kind=%llu status=%lld\n",
        (unsigned long long)result.kind, (long long)result.exit_status);
    ok = false;
  }
  status = pyxis_job_close(job);
  if (status != CALL_OK) {
    report_status("close worker scope", status);
    ok = false;
  }
  return ok;
}

static int parent(void)
{
  handle_t memory = HANDLE_INVALID, service = HANDLE_INVALID, launcher = HANDLE_INVALID;
  handle_t clock = HANDLE_INVALID, app = HANDLE_INVALID, bin = HANDLE_INVALID, image = HANDLE_INVALID;
  struct endpoint_create_reply channel = {0};
  struct shared_memory_created region = {0};
  struct memory_reservation view = {0};
  struct pyxis_job *job = NULL;
  uintmax_t bytes = 0, lines = 0;
  bool ok = false;
  if (!startup_capability("memory", &memory) || !startup_capability("service", &service) ||
      !startup_capability("launcher", &launcher) || !startup_capability("clock", &clock) ||
      !startup_capability("app", &app)) {
    goto done;
  }
  enum call_status status = directory_lookup(app, "bin", DIRECTORY_KIND_DIRECTORY,
      DIRECTORY_RIGHT_LOOKUP | DIRECTORY_RIGHT_READ_FILES, &bin);
  if (status == CALL_OK) {
    status = directory_lookup(bin, "shared-min.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &image);
  }
  if (status != CALL_OK) {
    report_status("hold app entry FILE", status);
    goto done;
  }
  if (pyxis_shared_memory_create(memory, sizeof(struct shared_buffer), &region) != 0 ||
      pyxis_shared_memory_map(region.handle, SHARED_MEMORY_RIGHTS, &view) != 0) {
    perror("shared-min: create/map");
    goto done;
  }
  if (view.size < sizeof(struct shared_buffer)) {
    fputs("shared-min: parent buffer is too small\n", stderr);
    goto done;
  }
  struct shared_buffer *buffer = (void *)(uintptr_t)view.address;
  atomic_init(&buffer->sequence, 0);
  status = endpoint_create(service, &channel);
  if (status != CALL_OK) {
    report_status("create endpoint", status);
    goto done;
  }
  struct launch_grant grants[] = {
    {memory, MEMORY_RIGHT_MANAGE, 0},
    {region.handle, SHARED_MEMORY_RIGHT_READ, 0},
    {channel.caller, 0, HANDLE_TRANSPORT_CALL},
  };
  struct launch_environment_entry entries[] = {
    {.name = (uintptr_t)"memory", .kind = ENVIRONMENT_CAP, .value = 0},
    {.name = (uintptr_t)"buffer", .kind = ENVIRONMENT_CAP, .value = 1},
    {.name = (uintptr_t)"caller", .kind = ENVIRONMENT_CAP, .value = 2},
  };
  const char *arguments[] = {"shared-min", "--worker"};
  struct pyxis_job_options options = {
    .image = image, .argv = arguments, .argc = 2,
    .grants = grants, .grant_count = sizeof(grants) / sizeof(*grants),
    .entries = entries, .entry_count = sizeof(entries) / sizeof(*entries),
    .streams = {-1, -1, 2},
  };
  status = pyxis_job_launch_contained(launcher, &options, &job);
  if (status != CALL_OK) {
    report_status("launch contained worker", status);
    goto done;
  }
  ok = transfer_input(channel.receiver, clock, job, buffer, &bytes, &lines);

done:
  if (!ok) {
    close_owned(&channel.receiver);
  }
  bool finished = finish_worker(&job, !ok);
  ok = finished && ok;
  ok = close_owned(&channel.receiver) && ok;
  /* Job close clears ownership only after confirmed scope retirement. */
  if (!job) {
    if (view.identity && pyxis_shared_memory_unmap(memory, view.identity) != 0) {
      perror("shared-min: unmap");
      ok = false;
    }
    ok = close_owned(&region.handle) && ok;
  } else {
    fputs("shared-min: scope cleanup unconfirmed; buffer left to process teardown\n", stderr);
  }
  ok = close_owned(&channel.caller) && ok;
  ok = close_owned(&image) && ok;
  ok = close_owned(&bin) && ok;
  ok = close_owned(&app) && ok;
  ok = close_owned(&clock) && ok;
  ok = close_owned(&launcher) && ok;
  ok = close_owned(&service) && ok;
  ok = close_owned(&memory) && ok;
  if (ok && (printf("bytes=%" PRIuMAX " lines=%" PRIuMAX "\n", bytes, lines) < 0 || fflush(stdout) != 0)) {
    fputs("shared-min: stdout write failed\n", stderr);
    ok = false;
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
  if (argc == 2 && !strcmp(argv[1], "--worker")) {
    return worker();
  }
  if (argc != 1) {
    fputs("usage: shared-min < input\n", stderr);
    return EXIT_FAILURE;
  }
  return parent();
}
