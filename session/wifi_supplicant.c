#include "wifi_supplicant.h"
#include "../common/launch_environment.h"
#include <abi/clock.h>
#include <abi/file.h>
#include <abi/memory.h>
#include <abi/random.h>
#include <clock.h>
#include <directory.h>
#include <environment.h>
#include <handle.h>
#include <launcher.h>
#include <startup.h>
#include <wait.h>
#include <wifi_auth.h>
#include <stdio.h>

#define CLAIM_TIMEOUT_NS UINT64_C(5000000000)
#define CLAIM_POLL_NS UINT64_C(10000000)
#define RETRY_MIN_NS UINT64_C(1000000000)
#define RETRY_MAX_NS UINT64_C(30000000000)
#define RETRY_RESET_NS UINT64_C(30000000000)
#define RETIRE_TIMEOUT_NS UINT64_C(5000000000)

static bool close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return true;
  }
  if (handle_close(*handle) != 0) {
    return false;
  }
  *handle = HANDLE_INVALID;
  return true;
}

static uint64_t deadline_after(uint64_t now, uint64_t duration)
{
  return UINT64_MAX - now < duration ? UINT64_MAX : now + duration;
}

bool session_wifi_active(const struct session_wifi_supplicant *worker)
{
  return worker->enabled || worker->group != HANDLE_INVALID;
}

static void disable_worker(struct session_wifi_supplicant *worker, enum call_status status)
{
  if (worker->enabled) {
    fprintf(stderr, "session: Wi-Fi authentication supervision disabled (status %u)\n", status);
  }
  worker->enabled = false;
}

static bool close_attempt(struct session_wifi_supplicant *worker)
{
  bool success = close_handle(&worker->auth);
  success &= close_handle(&worker->group_launcher);
  success &= close_handle(&worker->process);
  success &= close_handle(&worker->observation);
  success &= close_handle(&worker->group);
  worker->claimed = false;
  worker->stopping = false;
  return success;
}

static bool close_resources(struct session_wifi_supplicant *worker)
{
  bool success = close_handle(&worker->setup);
  success &= close_handle(&worker->launcher);
  success &= close_handle(&worker->image);
  success &= close_handle(&worker->memory);
  success &= close_handle(&worker->random);
  success &= close_handle(&worker->clock);
  return success;
}

static void schedule_retry(struct session_wifi_supplicant *worker, uint64_t now)
{
  worker->retry_deadline = deadline_after(now, worker->retry_delay);
  if (worker->retry_delay < RETRY_MAX_NS) {
    worker->retry_delay *= 2;
    if (worker->retry_delay > RETRY_MAX_NS) {
      worker->retry_delay = RETRY_MAX_NS;
    }
  }
}

static void retire_worker(struct session_wifi_supplicant *worker, uint64_t now)
{
  if (worker->group == HANDLE_INVALID || worker->stopping) {
    return;
  }
  if (worker->claimed && now >= worker->ready_since && now - worker->ready_since >= RETRY_RESET_NS) {
    worker->retry_delay = RETRY_MIN_NS;
  }
  worker->stopping = true;
  worker->retire_deadline = deadline_after(now, RETIRE_TIMEOUT_NS);
  enum call_status status = execution_group_terminate(worker->group);
  if (status != CALL_OK) {
    worker->uncertain = true;
    disable_worker(worker, status);
    fputs("session: Wi-Fi worker termination unconfirmed; retaining supervision\n", stderr);
  }
}

static void attempt_failed(struct session_wifi_supplicant *worker, enum call_status status, uint64_t now)
{
  fprintf(stderr, "session: Wi-Fi authentication worker unavailable (status %u)\n", status);
  bool closed = close_handle(&worker->auth);
  closed &= close_handle(&worker->group_launcher);
  if (!closed) {
    disable_worker(worker, CALL_BAD_HANDLE);
  }
  if (worker->group != HANDLE_INVALID) {
    retire_worker(worker, now);
  } else {
    if (!close_attempt(worker)) {
      disable_worker(worker, CALL_BAD_HANDLE);
    }
    schedule_retry(worker, now);
  }
}

static void start_attempt(struct session_wifi_supplicant *worker, uint64_t now)
{
  struct wifi_auth_open_reply pair = {0};
  enum call_status status = wifi_auth_open(worker->setup, &pair);
  if (status != CALL_OK) {
    if (status == CALL_DENIED || status == CALL_ENDPOINT_CLOSED || status == CALL_OUTCOME_UNKNOWN ||
        status == CALL_BAD_REQUEST || status == CALL_BAD_BUFFER || status == CALL_UNAVAILABLE) {
      disable_worker(worker, status);
    }
    attempt_failed(worker, status, now);
    return;
  }
  worker->auth = pair.auth;
  worker->observation = pair.observation;
  struct execution_group_create_reply group = {0};
  status = launcher_create_group(worker->launcher, &group);
  if (status != CALL_OK) {
    if (status == CALL_OUTCOME_UNKNOWN) {
      disable_worker(worker, status);
    }
    attempt_failed(worker, status, now);
    return;
  }
  worker->group = group.supervision;
  worker->group_launcher = group.launcher;
  struct launch_grant grants[] = {
    {worker->auth, WIFI_AUTH_RIGHT_RUN, 0}, {worker->memory, MEMORY_RIGHT_MANAGE, 0},
    {worker->clock, CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE, 0}, {worker->random, RANDOM_RIGHT_READ, 0},
  };
  struct launch_environment_entry entries[] = {
    PROFILE_PRIVATE_CAP("wifi_auth", 0), PROFILE_CAP("memory", 1),
    PROFILE_CAP("clock", 2), PROFILE_CAP("random", 3),
  };
  const char *arguments[] = {"boot://wifi-supplicant.pxe"};
  struct launch_request request = {
    .image = worker->image, .grants = (uintptr_t)grants, .grant_count = 4,
    .entries = (uintptr_t)entries, .entry_count = 4,
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  status = launcher_launch(worker->group_launcher, &request, &worker->process);
  if (status == CALL_OK) {
    status = execution_group_seal(worker->group);
  }
  if (status != CALL_OK) {
    /* Even an unknown launch outcome is contained by this known group. */
    attempt_failed(worker, status, now);
    return;
  }
  bool closed = close_handle(&worker->auth);
  closed &= close_handle(&worker->group_launcher);
  if (!closed) {
    attempt_failed(worker, CALL_BAD_HANDLE, now);
    return;
  }
  worker->claim_deadline = deadline_after(now, CLAIM_TIMEOUT_NS);
}

bool session_wifi_wait(struct session_wifi_supplicant *worker, uint64_t deadline)
{
  if (!session_wifi_active(worker)) {
    return true;
  }
  uint64_t now;
  enum call_status status = clock_now(worker->clock, &now);
  if (status != CALL_OK) {
    disable_worker(worker, status);
    retire_worker(worker, 0);
    return false;
  }
  if (worker->enabled && worker->group == HANDLE_INVALID && now >= worker->retry_deadline) {
    start_attempt(worker, now);
  }
  if (worker->group == HANDLE_INVALID) {
    if (!worker->enabled) {
      return true;
    }
    if (worker->retry_deadline < deadline) {
      deadline = worker->retry_deadline;
    }
    status = clock_sleep_until(worker->clock, deadline);
    if (status != CALL_OK) {
      disable_worker(worker, status);
    }
    return status == CALL_OK;
  }
  if (!worker->stopping && !worker->claimed) {
    struct wifi_auth_status_reply snapshot;
    status = wifi_auth_status(worker->observation, &snapshot);
    if (status != CALL_OK || (snapshot.flags & WIFI_AUTH_CLOSED)) {
      attempt_failed(worker, status != CALL_OK ? status : snapshot.status, now);
    } else if (snapshot.flags & WIFI_AUTH_CLAIMED) {
      worker->claimed = true;
      worker->ready_since = now;
    } else if (now >= worker->claim_deadline) {
      attempt_failed(worker, CALL_TIMED_OUT, now);
    }
  }
  if (worker->stopping && !worker->uncertain && now >= worker->retire_deadline) {
    worker->uncertain = true;
    disable_worker(worker, CALL_TIMED_OUT);
    fputs("session: Wi-Fi worker drain unconfirmed; retaining supervision\n", stderr);
  }
  struct wait_interest interests[3];
  size_t count = 0;
  if (!worker->stopping) {
    interests[count++] = (struct wait_interest){.handle = worker->observation, .events = WAIT_CLOSED};
    interests[count++] = (struct wait_interest){.handle = worker->process, .events = WAIT_COMPLETE};
    if (!worker->claimed) {
      uint64_t poll = deadline_after(now, CLAIM_POLL_NS);
      if (poll > worker->claim_deadline) {
        poll = worker->claim_deadline;
      }
      if (poll < deadline) {
        deadline = poll;
      }
    }
  } else if (!worker->uncertain && worker->retire_deadline < deadline) {
    deadline = worker->retire_deadline;
  }
  interests[count++] = (struct wait_interest){.handle = worker->group, .events = WAIT_COMPLETE};
  uint64_t maximum = deadline_after(now, WAIT_MAX_WAIT_NS);
  if (maximum < deadline) {
    deadline = maximum;
  }
  uint64_t events[3];
  status = wait_many(interests, count, deadline, events);
  if (status == CALL_TIMED_OUT) {
    return true;
  }
  if (status != CALL_OK) {
    /* A failed readiness call does not prove member or cleanup retirement. */
    disable_worker(worker, status);
    retire_worker(worker, now);
    worker->uncertain = true;
    return false;
  }
  status = clock_now(worker->clock, &now);
  if (status != CALL_OK) {
    disable_worker(worker, status);
    retire_worker(worker, 0);
    return false;
  }
  if (events[count - 1] & WAIT_COMPLETE) {
    status = execution_group_wait(worker->group);
    if (status != CALL_OK) {
      disable_worker(worker, status);
      worker->uncertain = true;
      return false;
    }
    if (worker->claimed && now >= worker->ready_since && now - worker->ready_since >= RETRY_RESET_NS) {
      worker->retry_delay = RETRY_MIN_NS;
    }
    if (!close_attempt(worker)) {
      disable_worker(worker, CALL_BAD_HANDLE);
    }
    schedule_retry(worker, now);
  } else if (!worker->stopping && (events[0] || events[1])) {
    attempt_failed(worker, CALL_ENDPOINT_CLOSED, now);
  }
  return true;
}

bool session_wifi_stop(struct session_wifi_supplicant *worker)
{
  worker->enabled = false;
  if (worker->group != HANDLE_INVALID) {
    uint64_t now;
    if (clock_now(worker->clock, &now) != CALL_OK) {
      retire_worker(worker, 0);
      return false;
    }
    retire_worker(worker, now);
    while (worker->group != HANDLE_INVALID && !worker->uncertain) {
      if (!session_wifi_wait(worker, deadline_after(now, WAIT_MAX_WAIT_NS)) ||
          clock_now(worker->clock, &now) != CALL_OK) {
        return false;
      }
    }
    if (worker->group != HANDLE_INVALID) {
      return false;
    }
  }
  bool success = close_attempt(worker);
  success &= close_resources(worker);
  return success;
}

static enum call_status retain_resource(const struct environment_capture *capture,
    const char *name, uint64_t rights, handle_t *handle)
{
  const struct environment_value *value = environment_capture_find(capture, name);
  if (!value || value->kind != ENVIRONMENT_CAP || value->transport || (value->rights & rights) != rights) {
    return CALL_DENIED;
  }
  return handle_copy_restricted(value->handle, rights, 0, handle);
}

void session_wifi_start(struct session_wifi_supplicant *worker)
{
  *worker = (struct session_wifi_supplicant){.retry_delay = RETRY_MIN_NS};
  handle_t table = startup_environment_handle();
  struct environment_capture capture = {0};
  enum call_status status = environment_capture_init(table,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status != CALL_OK) {
    fprintf(stderr, "session: Wi-Fi setup unavailable (status %u)\n", status);
    return;
  }
  const struct environment_value *setup = environment_capture_find(&capture, "wifi_auth_setup");
  if (!setup) {
    environment_capture_close(&capture);
    return;
  }
  struct handle_info info;
  status = setup->kind == ENVIRONMENT_CAP ? handle_query(setup->handle, &info) : CALL_WRONG_TYPE;
  if (status == CALL_OK && (setup->flags || info.kind != HANDLE_KIND_NATIVE ||
      info.protocol != PROTOCOL_WIFI || info.transport || info.rights != WIFI_RIGHT_SUPPLICANT)) {
    status = CALL_DENIED;
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(setup->handle, WIFI_RIGHT_SUPPLICANT, 0, &worker->setup);
  }
  if (status == CALL_OK) {
    struct environment_unset_entry removed = {.identity = setup->identity};
    environment_name(removed.name, "wifi_auth_setup");
    status = environment_unset_batch(table, &removed, 1);
  }
  if (status == CALL_OK) {
    status = retain_resource(&capture, "launcher", LAUNCHER_RIGHT_LAUNCH, &worker->launcher);
  }
  if (status == CALL_OK) {
    status = retain_resource(&capture, "memory", MEMORY_RIGHT_MANAGE, &worker->memory);
  }
  if (status == CALL_OK) {
    status = retain_resource(&capture, "clock", CLOCK_RIGHT_READ | CLOCK_RIGHT_PRECISE | CLOCK_RIGHT_SLEEP, &worker->clock);
  }
  if (status == CALL_OK) {
    status = retain_resource(&capture, "random", RANDOM_RIGHT_READ, &worker->random);
  }
  /* The supplicant is in the core archive (docs/wip/pkg-upgrade.md, decision 18). */
  const struct environment_value *boot = environment_capture_find(&capture, "boot");
  if (status == CALL_OK) {
    status = boot && boot->kind == ENVIRONMENT_CAP ?
        directory_lookup(boot->handle, "wifi-supplicant.pxe", DIRECTORY_KIND_FILE, FILE_RIGHT_READ, &worker->image) :
        CALL_DENIED;
  }
  uint64_t now = 0;
  if (status == CALL_OK) {
    status = clock_now(worker->clock, &now);
  }
  environment_capture_close(&capture);
  if (status != CALL_OK) {
    fprintf(stderr, "session: Wi-Fi setup unavailable (status %u)\n", status);
    close_resources(worker);
    return;
  }
  worker->enabled = true;
  worker->retry_deadline = now;
  start_attempt(worker, now);
}
