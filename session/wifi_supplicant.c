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

static bool close_handle(handle_t *handle)
{
  bool success = *handle == HANDLE_INVALID || handle_close(*handle) == 0;
  *handle = HANDLE_INVALID;
  return success;
}

bool session_wifi_active(const struct session_wifi_supplicant *worker)
{
  return worker->group != HANDLE_INVALID;
}

static bool retire_worker(struct session_wifi_supplicant *worker)
{
  if (!session_wifi_active(worker)) {
    return true;
  }
  if (!worker->stopping) {
    if (execution_group_terminate(worker->group) != CALL_OK) {
      return false;
    }
    worker->stopping = true;
  }
  return true;
}

bool session_wifi_wait(struct session_wifi_supplicant *worker, uint64_t deadline)
{
  if (!session_wifi_active(worker)) {
    return true;
  }
  struct wait_interest interests[3];
  size_t count = 0;
  if (!worker->stopping) {
    interests[count++] = (struct wait_interest){.handle = worker->observation, .events = WAIT_CLOSED};
    interests[count++] = (struct wait_interest){.handle = worker->process, .events = WAIT_COMPLETE};
  }
  interests[count++] = (struct wait_interest){.handle = worker->group, .events = WAIT_COMPLETE};
  uint64_t events[3];
  enum call_status status = wait_many(interests, count, deadline, events);
  if (status == CALL_TIMED_OUT) {
    return true;
  }
  if (status != CALL_OK) {
    if (!worker->stopping && (status == CALL_DENIED || status == CALL_ENDPOINT_CLOSED)) {
      struct wifi_auth_status_reply snapshot;
      enum call_status observed = wifi_auth_status(worker->observation, &snapshot);
      if (observed == CALL_DENIED || observed == CALL_ENDPOINT_CLOSED ||
          (observed == CALL_OK && (snapshot.flags & WIFI_AUTH_CLOSED))) {
        return retire_worker(worker);
      }
    }
    return false;
  }
  bool complete = (events[count - 1] & WAIT_COMPLETE) != 0;
  if (!worker->stopping && (events[0] || events[1]) && !retire_worker(worker)) {
    return false;
  }
  if (complete) {
    if (execution_group_wait(worker->group) != CALL_OK) {
      return false;
    }
    bool success = close_handle(&worker->process);
    success &= close_handle(&worker->observation);
    success &= close_handle(&worker->group);
    success &= close_handle(&worker->clock);
    return success;
  }
  return true;
}

bool session_wifi_stop(struct session_wifi_supplicant *worker)
{
  if (!retire_worker(worker)) {
    return false;
  }
  while (session_wifi_active(worker)) {
    uint64_t now;
    if (clock_now(worker->clock, &now) != CALL_OK || now > UINT64_MAX - WAIT_MAX_WAIT_NS ||
        !session_wifi_wait(worker, now + WAIT_MAX_WAIT_NS)) {
      return false;
    }
  }
  return true;
}

bool session_wifi_start(struct session_wifi_supplicant *worker)
{
  *worker = (struct session_wifi_supplicant){0};
  handle_t table = startup_environment_handle();
  struct environment_capture capture = {0};
  enum call_status status = environment_capture_init(table,
      ENVIRONMENT_SNAPSHOT_INCLUDE_PRIVATE, &capture);
  if (status != CALL_OK) {
    return false;
  }
  const struct environment_value *auth = environment_capture_find(&capture, "wifi_auth");
  const struct environment_value *observation =
      environment_capture_find(&capture, "wifi_auth_observation");
  if (!auth && !observation) {
    environment_capture_close(&capture);
    return true;
  }
  handle_t image = HANDLE_INVALID;
  struct execution_group_create_reply group = {0};
  const struct environment_value *bin = environment_capture_find(&capture, "bin");
  const struct environment_value *launcher = environment_capture_find(&capture, "launcher");
  const struct environment_value *memory = environment_capture_find(&capture, "memory");
  const struct environment_value *clock = environment_capture_find(&capture, "clock");
  const struct environment_value *random = environment_capture_find(&capture, "random");
  if (!auth || !observation || !bin || !launcher || !memory || !clock || !random ||
      bin->kind != ENVIRONMENT_CAP || launcher->kind != ENVIRONMENT_CAP ||
      memory->kind != ENVIRONMENT_CAP || clock->kind != ENVIRONMENT_CAP ||
      random->kind != ENVIRONMENT_CAP || auth->kind != ENVIRONMENT_CAP || observation->kind != ENVIRONMENT_CAP ||
      auth->flags || observation->flags || auth->transport || observation->transport ||
      auth->rights != WIFI_AUTH_RIGHT_RUN || observation->rights != WIFI_AUTH_RIGHT_OBSERVE) {
    status = CALL_DENIED;
    goto done;
  }
  struct handle_info info;
  status = handle_query(auth->handle, &info);
  if (status != CALL_OK || info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_WIFI_AUTH) {
    if (status == CALL_OK) {
      status = CALL_WRONG_TYPE;
    }
    goto done;
  }
  status = handle_query(observation->handle, &info);
  if (status != CALL_OK || info.kind != HANDLE_KIND_NATIVE || info.protocol != PROTOCOL_WIFI_AUTH) {
    if (status == CALL_OK) {
      status = CALL_WRONG_TYPE;
    }
    goto done;
  }
  status = directory_lookup(bin->handle, "wifi-supplicant.pxe", DIRECTORY_KIND_FILE,
      FILE_RIGHT_READ, &image);
  if (status != CALL_OK) {
    goto done;
  }
  status = handle_copy_restricted(clock->handle, CLOCK_RIGHT_READ, 0, &worker->clock);
  if (status != CALL_OK) {
    goto done;
  }
  status = handle_copy(observation->handle, &worker->observation);
  if (status != CALL_OK) {
    goto done;
  }
  status = launcher_create_group(launcher->handle, &group);
  if (status != CALL_OK) {
    goto done;
  }
  worker->group = group.supervision;
  struct launch_grant grants[] = {
    {auth->handle, WIFI_AUTH_RIGHT_RUN, 0}, {memory->handle, MEMORY_RIGHT_MANAGE, 0},
    {clock->handle, CLOCK_RIGHT_READ, 0}, {random->handle, RANDOM_RIGHT_READ, 0},
  };
  struct launch_environment_entry entries[] = {
    PROFILE_PRIVATE_CAP("wifi_auth", 0), PROFILE_CAP("memory", 1),
    PROFILE_CAP("clock", 2), PROFILE_CAP("random", 3),
  };
  const char *arguments[] = {"bin://wifi-supplicant.pxe"};
  struct launch_request request = {
    .image = image, .grants = (uintptr_t)grants, .grant_count = 4,
    .entries = (uintptr_t)entries, .entry_count = 4,
    .argv = (uintptr_t)arguments, .argc = 1,
  };
  status = launcher_launch(group.launcher, &request, &worker->process);
  if (status != CALL_OK) {
    goto done;
  }
  status = execution_group_seal(worker->group);
  if (status != CALL_OK) {
    goto done;
  }
  struct environment_unset_entry removed[2] = {
    {.identity = auth->identity}, {.identity = observation->identity},
  };
  environment_name(removed[0].name, "wifi_auth");
  environment_name(removed[1].name, "wifi_auth_observation");
  status = environment_unset_batch(table, removed, 2);
  if (status != CALL_OK) {
    goto done;
  }
  uint64_t now;
  status = clock_now(worker->clock, &now);
  if (status != CALL_OK) {
    goto done;
  }
  if (now > UINT64_MAX - CLAIM_TIMEOUT_NS) {
    status = CALL_LIMIT;
    goto done;
  }
  uint64_t deadline = now + CLAIM_TIMEOUT_NS;
  for (;;) {
    struct wifi_auth_status_reply snapshot;
    status = wifi_auth_status(worker->observation, &snapshot);
    if (status != CALL_OK) {
      goto done;
    }
    if (snapshot.flags & WIFI_AUTH_CLOSED) {
      status = CALL_ENDPOINT_CLOSED;
      goto done;
    }
    if (snapshot.flags & WIFI_AUTH_CLAIMED) {
      break;
    }
    status = clock_now(worker->clock, &now);
    if (status != CALL_OK) {
      goto done;
    }
    if (now >= deadline) {
      status = CALL_TIMED_OUT;
      goto done;
    }
    uint64_t wake = deadline - now < CLAIM_POLL_NS ? deadline : now + CLAIM_POLL_NS;
    struct wait_interest interest = {.handle = worker->process, .events = WAIT_COMPLETE};
    uint64_t events;
    status = wait_many(&interest, 1, wake, &events);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      goto done;
    }
    if (status == CALL_OK && events) {
      status = CALL_ENDPOINT_CLOSED;
      goto done;
    }
  }
  status = CALL_OK;

done:
  bool closed = close_handle(&image);
  closed &= close_handle(&group.launcher);
  if (!closed && status == CALL_OK) {
    status = CALL_BAD_HANDLE;
  }
  environment_capture_close(&capture);
  if (status != CALL_OK) {
    fprintf(stderr, "session: cannot start Wi-Fi authentication worker (status %u)\n", status);
    bool drained = session_wifi_stop(worker);
    if (!session_wifi_active(worker)) {
      close_handle(&worker->observation);
      close_handle(&worker->clock);
    }
    if (!drained) {
      fputs("session: Wi-Fi authentication cleanup not confirmed\n", stderr);
    }
  }
  return status == CALL_OK;
}
