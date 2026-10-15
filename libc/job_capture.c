#include <environment.h>
#include <pyxis/capture.h>
#include <abi/pipe.h>
#include <output.h>
#include <pyxis/descriptor.h>
#include <abi/clock.h>
#include <clock.h>
#include <file.h>
#include <handle.h>
#include <syscall.h>
#include <wait.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "job_internal.h"

struct pyxis_capture {
  struct pyxis_job *job;
  handle_t clock, output_service, launcher, supervision;
  handle_t values[2], observations[2], collectors[2], producers[2];
  bool sealed[2], collected[2], take_uncertain[2];
  uint64_t limits[2], deadline, poll_interval, next_poll;
  bool stop_requested, group_complete, uncertain;
  struct pyxis_capture_result result;
};

static uint64_t add_deadline(uint64_t now, uint64_t duration)
{
  return duration > UINT64_MAX - now ? UINT64_MAX : now + duration;
}

static void set_stop(struct pyxis_capture *capture, enum pyxis_capture_stop stop)
{
  if (capture->result.stop == PYXIS_CAPTURE_STOP_NONE) {
    capture->result.stop = stop;
  }
}

static enum call_status remember_error(struct pyxis_capture *capture, enum call_status status)
{
  if (capture->result.error_status == CALL_OK) {
    capture->result.error_status = status;
  }
  set_stop(capture, PYXIS_CAPTURE_STOP_ERROR);
  return status;
}

static enum call_status close_handle(handle_t *handle)
{
  if (*handle == HANDLE_INVALID) {
    return CALL_OK;
  }
  struct syscall_result result = syscall_close(*handle);
  *handle = HANDLE_INVALID;
  return result.status >= CALL_STATUS_COUNT || result.reply_size ?
      CALL_OUTCOME_UNKNOWN : result.status;
}

static enum call_status discard_capture(struct pyxis_capture *capture)
{
  enum call_status error = CALL_OK;
  if (capture->job) {
    error = pyxis_job_close(&capture->job);
  }
  handle_t *handles[] = {&capture->values[0], &capture->values[1],
    &capture->observations[0], &capture->observations[1],
    &capture->collectors[0], &capture->collectors[1],
    &capture->producers[0], &capture->producers[1],
    &capture->supervision, &capture->launcher, &capture->output_service, &capture->clock};
  for (size_t i = 0; i < sizeof(handles) / sizeof(*handles); ++i) {
    enum call_status status = close_handle(handles[i]);
    if (error == CALL_OK) {
      error = status;
    }
  }
  free(capture);
  return error;
}

static enum call_status request_stop(struct pyxis_capture *capture)
{
  if (capture->result.complete || capture->stop_requested) {
    return CALL_OK;
  }
  if (capture->uncertain && !capture->supervision) {
    return CALL_OUTCOME_UNKNOWN;
  }
  enum call_status status = capture->supervision ?
      execution_group_terminate(capture->supervision) :
      capture->job ? pyxis_job_terminate(capture->job) : CALL_OK;
  if (status == CALL_OK) {
    capture->stop_requested = true;
  } else {
    remember_error(capture, status);
  }
  return status;
}

static size_t interests(struct pyxis_capture *capture, struct wait_interest waits[4])
{
  size_t count = 0;
  if (capture->job && !capture->result.result_valid) {
    waits[count++] = (struct wait_interest){pyxis_job_observer(capture->job), WAIT_COMPLETE, 0};
  }
  if (capture->supervision && !capture->group_complete) {
    waits[count++] = (struct wait_interest){capture->supervision, WAIT_COMPLETE, 0};
  }
  if (capture->result.complete) {
    for (size_t i = 0; i < 2; ++i) {
      if (capture->sealed[i] && !capture->collected[i] && !capture->take_uncertain[i]) {
        waits[count++] = (struct wait_interest){capture->collectors[i], WAIT_COMPLETE | WAIT_CLOSED, 0};
      }
    }
  }
  return count;
}

static enum call_status final_outputs(struct pyxis_capture *capture)
{
  struct pyxis_capture_output *outputs[] = {
    &capture->result.stdout_output, &capture->result.stderr_output,
  };
  for (size_t i = 0; i < 2; ++i) {
    if (outputs[i]->valid) {
      continue;
    }
    enum call_status status;
    if (capture->take_uncertain[i]) {
      return CALL_OUTCOME_UNKNOWN;
    }
    if (!capture->sealed[i]) {
      status = output_seal(capture->collectors[i]);
      if (status != CALL_OK) {
        return remember_error(capture, status);
      }
      capture->sealed[i] = true;
    }
    struct output_status_reply state;
    status = output_status(capture->collectors[i], &state);
    if (status != CALL_OK) {
      return remember_error(capture, status);
    }
    if (!capture->collected[i]) {
      struct output_take_reply taken;
      status = output_take(capture->collectors[i], &taken);
      if (status == CALL_WOULD_BLOCK) {
        return status;
      }
      if (status != CALL_OK) {
        capture->take_uncertain[i] = status == CALL_OUTCOME_UNKNOWN;
        return remember_error(capture, status);
      }
      capture->values[i] = taken.value;
      capture->observations[i] = taken.observation;
      capture->collected[i] = true;
    }
    uint64_t size = 0;
    if (state.mode != OUTPUT_MODE_VALUE) {
      status = file_size(capture->values[i], &size);
      if (status != CALL_OK) {
        return remember_error(capture, status);
      }
    }
    *outputs[i] = (struct pyxis_capture_output){
      .valid = true, .mode = state.mode, .size = size,
      .truncated = state.mode != OUTPUT_MODE_VALUE && size > capture->limits[i],
      .returned_size = size < capture->limits[i] ? size : capture->limits[i],
    };
  }
  return CALL_OK;
}

static enum call_status advance(struct pyxis_capture *capture)
{
  if (capture->result.complete) {
    return final_outputs(capture);
  }
  if (capture->uncertain && !capture->supervision) {
    return CALL_OUTCOME_UNKNOWN;
  }
  uint64_t now = 0;
  enum call_status status = CALL_OK;
  if (capture->job && !capture->result.result_valid &&
      capture->result.stop == PYXIS_CAPTURE_STOP_NONE) {
    status = clock_now(capture->clock, &now);
    if (status != CALL_OK) {
      remember_error(capture, status);
      request_stop(capture);
      return status;
    }
    if (now >= capture->deadline) {
      set_stop(capture, PYXIS_CAPTURE_STOP_DEADLINE);
    }
  }
  if (capture->result.stop != PYXIS_CAPTURE_STOP_NONE) {
    status = request_stop(capture);
    if (status != CALL_OK) {
      return status;
    }
  }

  struct wait_interest waits[4];
  uint64_t events[4] = {0};
  size_t count = interests(capture, waits);
  if (count) {
    status = wait_many(waits, count, 0, events);
    if (status != CALL_OK) {
      remember_error(capture, status);
      request_stop(capture);
      return status;
    }
    for (size_t i = 0; i < count; ++i) {
      if (!(events[i] & WAIT_COMPLETE)) {
        continue;
      }
      if (capture->supervision && waits[i].handle == capture->supervision) {
        status = execution_group_wait(capture->supervision);
        if (status == CALL_OK) {
          capture->group_complete = true;
        }
      } else {
        status = pyxis_job_wait(capture->job, &capture->result.process);
        if (status == CALL_OK) {
          capture->result.result_valid = true;
        }
      }
      if (status != CALL_OK) {
        remember_error(capture, status);
        request_stop(capture);
        return status;
      }
    }
  }
  if (capture->group_complete && capture->job && !capture->result.result_valid) {
    status = pyxis_job_wait(capture->job, &capture->result.process);
    if (status != CALL_OK) {
      return remember_error(capture, status);
    }
    capture->result.result_valid = true;
  }
  if ((!capture->job || capture->result.result_valid) &&
      (!capture->supervision || capture->group_complete)) {
    capture->result.complete = true;
    capture->result.cleanup_unconfirmed = false;
    return final_outputs(capture);
  }

  if (capture->job && !capture->result.result_valid &&
      capture->result.stop == PYXIS_CAPTURE_STOP_NONE && now >= capture->next_poll) {
    for (size_t i = 0; i < 2; ++i) {
      struct output_status_reply state;
      status = output_status(capture->collectors[i], &state);
      if (status == CALL_OK && (state.flags & OUTPUT_FAILED)) {
        status = CALL_IO;
      }
      if (status == CALL_OK && state.mode != OUTPUT_MODE_BYTES) {
        continue;
      }
      uint64_t size = 0;
      if (status == CALL_OK) {
        status = file_size(capture->producers[i], &size);
      }
      if (status != CALL_OK) {
        remember_error(capture, status);
        request_stop(capture);
        return status;
      }
      if (size >= capture->limits[i]) {
        set_stop(capture, i ? PYXIS_CAPTURE_STOP_STDERR_LIMIT : PYXIS_CAPTURE_STOP_STDOUT_LIMIT);
      }
    }
    capture->next_poll = add_deadline(now, capture->poll_interval);
    if (capture->result.stop != PYXIS_CAPTURE_STOP_NONE) {
      return request_stop(capture);
    }
  }
  return CALL_OK;
}

static enum call_status validate_source(handle_t handle, uint64_t protocol, uint64_t rights)
{
  struct handle_info info;
  enum call_status status = handle_query(handle, &info);
  if (status != CALL_OK) {
    return status;
  }
  if (info.kind != HANDLE_KIND_NATIVE || info.protocol != protocol || info.transport) {
    return CALL_WRONG_TYPE;
  }
  return rights & ~info.rights ? CALL_DENIED : CALL_OK;
}

static enum call_status validate_options(handle_t launcher,
    const struct pyxis_capture_options *options)
{
  if (!options || !options->deadline_ns ||
      options->command.grant_count > LAUNCH_CAPTURE_MAX_SIZE / sizeof(struct launch_grant) ||
      (options->command.grant_count && !options->command.grants)) {
    return CALL_BAD_REQUEST;
  }
  for (size_t i = 0; i < ENVIRONMENT_STREAM_COUNT; ++i) {
    if (options->command.streams[i] != -1) {
      return CALL_BAD_REQUEST;
    }
  }
  enum call_status status = validate_source(launcher, PROTOCOL_LAUNCHER,
      LAUNCHER_RIGHT_LAUNCH);
  if (status != CALL_OK) {
    return status;
  }
  status = validate_source(options->clock, PROTOCOL_CLOCK, CLOCK_RIGHT_READ);
  if (status != CALL_OK) {
    return status;
  }
  status = validate_source(options->output_service, PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE);
  if (status != CALL_OK) {
    return status;
  }
  return libc_job_validate_capture_grants(options->command.grants,
      options->command.grant_count, options->output_service);
}

enum call_status pyxis_capture_start(handle_t launcher,
    const struct pyxis_capture_options *options, struct pyxis_capture **capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  *capture = NULL;
  enum call_status status = validate_options(launcher, options);
  if (status != CALL_OK) {
    return status;
  }
  struct pyxis_capture *owned = calloc(1, sizeof(*owned));
  if (!owned) {
    return CALL_NO_MEMORY;
  }
  owned->deadline = options->deadline_ns;
  owned->poll_interval = options->poll_interval_ns ? options->poll_interval_ns :
      PYXIS_CAPTURE_DEFAULT_POLL_NS;
  owned->limits[0] = options->stdout_limit ? options->stdout_limit : PYXIS_CAPTURE_DEFAULT_LIMIT;
  owned->limits[1] = options->stderr_limit ? options->stderr_limit : PYXIS_CAPTURE_DEFAULT_LIMIT;
  status = handle_copy_restricted(options->clock, CLOCK_RIGHT_READ, 0, &owned->clock);
  if (status == CALL_OK) {
    status = handle_copy_restricted(options->output_service, PIPE_SERVICE_RIGHT_CREATE, 0, &owned->output_service);
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(launcher, LAUNCHER_RIGHT_LAUNCH, 0, &owned->launcher);
  }
  uint64_t now;
  if (status == CALL_OK) {
    status = clock_now(owned->clock, &now);
    if (status == CALL_OK && now >= owned->deadline) {
      status = CALL_TIMED_OUT;
    }
  }
  int writers[2] = {-1, -1};
  for (size_t i = 0; i < 2 && status == CALL_OK; ++i) {
    struct output_create_reply pair;
    status = output_create(owned->output_service, &pair);
    if (status == CALL_OK) {
      owned->collectors[i] = pair.collector;
      owned->producers[i] = pair.producer;
      handle_t writer = HANDLE_INVALID;
      status = handle_copy_restricted(pair.producer, FILE_RIGHT_WRITE, 0, &writer);
      if (status == CALL_OK) {
        writers[i] = pyxis_descriptor_adopt(&writer, PYXIS_DESCRIPTOR_WRITE);
        if (writers[i] < 0) {
          status = CALL_NO_MEMORY;
          close_handle(&writer);
        }
      }
    }
  }
  if (status == CALL_OK && options->fresh_group) {
    struct execution_group_create_reply group = {0};
    status = launcher_create_group(owned->launcher, &group);
    if (status == CALL_OK) {
      owned->supervision = group.supervision;
      status = close_handle(&owned->launcher);
      owned->launcher = group.launcher;
    } else if (status == CALL_OUTCOME_UNKNOWN) {
      owned->uncertain = true;
    }
  }
  if (status == CALL_OK) {
    struct pyxis_job_options command = options->command;
    command.streams[ENVIRONMENT_STDOUT] = writers[0];
    command.streams[ENVIRONMENT_STDERR] = writers[1];
    status = libc_job_launch_captured(owned->launcher, &command, owned->output_service, &owned->job);
    if (status == CALL_OUTCOME_UNKNOWN) {
      owned->uncertain = true;
    }
  }
  int saved_errno = errno;
  enum call_status writer_error = CALL_OK;
  for (size_t i = 0; i < 2; ++i) {
    if (writers[i] >= 0 && close(writers[i]) < 0) {
      writer_error = CALL_IO;
    }
  }
  errno = saved_errno;
  owned->result.start_status = status;
  if (status != CALL_OK) {
    remember_error(owned, status);
    if (!owned->supervision && !owned->uncertain) {
      discard_capture(owned);
      return status;
    }
    owned->result.cleanup_unconfirmed = owned->uncertain;
    request_stop(owned);
  } else if (owned->supervision) {
    status = execution_group_seal(owned->supervision);
    if (status != CALL_OK) {
      remember_error(owned, status);
      request_stop(owned);
    }
  }
  if (writer_error != CALL_OK) {
    remember_error(owned, writer_error);
    request_stop(owned);
    if (status == CALL_OK) {
      status = writer_error;
    }
  }
  *capture = owned;
  if (status == CALL_OK) {
    status = advance(owned);
  }
  return status;
}

enum call_status pyxis_capture_step(struct pyxis_capture *capture,
    struct pyxis_capture_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct pyxis_capture_result){0};
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  enum call_status status = advance(capture);
  *result = capture->result;
  return status;
}

enum call_status pyxis_capture_wait(struct pyxis_capture *capture,
    struct pyxis_capture_result *result)
{
  if (!result || !capture) {
    if (result) {
      *result = (struct pyxis_capture_result){0};
    }
    return CALL_BAD_REQUEST;
  }
  for (;;) {
    enum call_status status = pyxis_capture_step(capture, result);
    if ((status != CALL_OK && status != CALL_WOULD_BLOCK) ||
        (status == CALL_OK && result->complete)) {
      return status;
    }
    uint64_t now;
    status = clock_now(capture->clock, &now);
    if (status != CALL_OK) {
      remember_error(capture, status);
      request_stop(capture);
      *result = capture->result;
      return status;
    }
    uint64_t until = add_deadline(now, capture->poll_interval);
    if (capture->job && !capture->result.result_valid &&
        capture->result.stop == PYXIS_CAPTURE_STOP_NONE) {
      if (capture->next_poll < until) {
        until = capture->next_poll;
      }
      if (capture->deadline < until) {
        until = capture->deadline;
      }
    }
    uint64_t maximum = add_deadline(now, WAIT_MAX_WAIT_NS);
    if (until > maximum) {
      until = maximum;
    }
    struct wait_interest waits[4];
    uint64_t events[4];
    size_t count = interests(capture, waits);
    status = wait_many(waits, count, until, events);
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      remember_error(capture, status);
      request_stop(capture);
      *result = capture->result;
      return status;
    }
  }
}

enum call_status pyxis_capture_cancel(struct pyxis_capture *capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  if (!capture->result.complete) {
    set_stop(capture, PYXIS_CAPTURE_STOP_CANCEL);
  }
  return request_stop(capture);
}

enum call_status pyxis_capture_read(struct pyxis_capture *capture,
    enum environment_stream_index stream, uint64_t offset, void *bytes,
    size_t capacity, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  if (!capture || (stream != ENVIRONMENT_STDOUT && stream != ENVIRONMENT_STDERR) ||
      (capacity && !bytes)) {
    return CALL_BAD_REQUEST;
  }
  if (!capture->result.complete) {
    return CALL_BUSY;
  }
  struct pyxis_capture_output *output = stream == ENVIRONMENT_STDOUT ?
      &capture->result.stdout_output : &capture->result.stderr_output;
  if (!output->valid || output->taken) {
    return CALL_UNAVAILABLE;
  }
  if (output->mode != OUTPUT_MODE_VALUE && offset >= output->returned_size) {
    capacity = 0;
  } else if (output->mode != OUTPUT_MODE_VALUE && capacity > output->returned_size - offset) {
    capacity = output->returned_size - offset;
  }
  enum call_status status = file_read(capture->values[stream - ENVIRONMENT_STDOUT],
      offset, bytes, capacity, read);
  return status == CALL_OK && output->mode != OUTPUT_MODE_VALUE && capacity && !*read ? CALL_IO : status;
}

enum call_status pyxis_capture_take(struct pyxis_capture *capture,
    enum environment_stream_index stream, struct output_take_reply *owned)
{
  if (!capture || !owned ||
      (stream != ENVIRONMENT_STDOUT && stream != ENVIRONMENT_STDERR)) {
    return CALL_BAD_REQUEST;
  }
  if (!capture->result.complete) {
    return CALL_BUSY;
  }
  size_t index = stream - ENVIRONMENT_STDOUT;
  struct pyxis_capture_output *output = index ?
      &capture->result.stderr_output : &capture->result.stdout_output;
  if (!output->valid || output->taken) {
    return CALL_UNAVAILABLE;
  }
  *owned = (struct output_take_reply){capture->values[index], capture->observations[index]};
  capture->values[index] = HANDLE_INVALID;
  capture->observations[index] = HANDLE_INVALID;
  output->taken = true;
  return CALL_OK;
}

enum call_status pyxis_capture_close(struct pyxis_capture **capture)
{
  if (!capture) {
    return CALL_BAD_REQUEST;
  }
  if (!*capture) {
    return CALL_OK;
  }
  if (!(*capture)->result.complete) {
    return CALL_BUSY;
  }
  struct pyxis_capture *owned = *capture;
  *capture = NULL;
  return discard_capture(owned);
}
