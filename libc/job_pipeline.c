#include <environment.h>
#include <pyxis/pipeline.h>
#include <abi/directory.h>
#include <clock.h>
#include <file.h>
#include <handle.h>
#include <pipe.h>
#include <syscall.h>
#include <wait.h>
#include <stdlib.h>
#include <string.h>
#include "job_capture_files.h"
#include "job_pipeline_launch.h"

struct pipeline_output {
  handle_t reader, file;
  unsigned char pending[FILE_WRITE_MAX_BYTES];
  size_t pending_size, pending_offset;
  uint64_t written;
};

struct pyxis_pipeline {
  handle_t clock, private_root, pipe_service, launcher, supervision;
  handle_t observers[LAUNCH_BATCH_MAX];
  bool stop_requested[LAUNCH_BATCH_MAX], group_stopped, group_complete, uncertain;
  handle_t input_writer;
  unsigned char *input;
  uint64_t deadline, limits[2];
  struct pipeline_output outputs[2];
  struct pyxis_pipeline_result result;
};

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

static void set_stop(struct pyxis_pipeline *pipeline, enum pyxis_capture_stop stop)
{
  if (pipeline->result.stop == PYXIS_CAPTURE_STOP_NONE) {
    pipeline->result.stop = stop;
  }
}

static enum call_status remember_error(struct pyxis_pipeline *pipeline, enum call_status status)
{
  if (status != CALL_OK) {
    if (pipeline->result.error_status == CALL_OK) {
      pipeline->result.error_status = status;
    }
    set_stop(pipeline, PYXIS_CAPTURE_STOP_ERROR);
  }
  return status;
}

static enum call_status close_input(struct pyxis_pipeline *pipeline)
{
  enum call_status status = close_handle(&pipeline->input_writer);
  pipeline->result.input_closed = true;
  return remember_error(pipeline, status);
}

static enum call_status discard_pipeline(struct pyxis_pipeline *pipeline)
{
  enum call_status error = CALL_OK;
  handle_t *handles[LAUNCH_BATCH_MAX + 10];
  size_t count = 0;
  for (size_t i = 0; i < pipeline->result.stage_count; ++i) {
    handles[count++] = &pipeline->observers[i];
  }
  handles[count++] = &pipeline->input_writer;
  for (size_t i = 0; i < 2; ++i) {
    handles[count++] = &pipeline->outputs[i].reader;
    handles[count++] = &pipeline->outputs[i].file;
  }
  handles[count++] = &pipeline->supervision;
  handles[count++] = &pipeline->launcher;
  handles[count++] = &pipeline->pipe_service;
  handles[count++] = &pipeline->private_root;
  handles[count++] = &pipeline->clock;
  for (size_t i = 0; i < count; ++i) {
    enum call_status status = close_handle(handles[i]);
    if (error == CALL_OK) {
      error = status;
    }
  }
  free(pipeline->input);
  free(pipeline);
  return error;
}

static bool stages_retired(const struct pyxis_pipeline *pipeline)
{
  for (size_t i = 0; i < pipeline->result.stage_count; ++i) {
    if (pipeline->observers[i] && !pipeline->result.stage_valid[i]) {
      return false;
    }
  }
  return !pipeline->uncertain || pipeline->group_complete;
}

static enum call_status request_stop(struct pyxis_pipeline *pipeline)
{
  enum call_status error = close_input(pipeline);
  if (pipeline->uncertain && !pipeline->supervision) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (pipeline->supervision) {
    if (!pipeline->group_stopped && !pipeline->group_complete) {
      enum call_status status = execution_group_terminate(pipeline->supervision);
      if (status == CALL_OK) {
        pipeline->group_stopped = true;
      }
      if (error == CALL_OK) {
        error = status;
      }
    }
  } else {
    for (size_t i = 0; i < pipeline->result.stage_count; ++i) {
      if (!pipeline->observers[i] || pipeline->stop_requested[i] ||
          pipeline->result.stage_valid[i]) {
        continue;
      }
      enum call_status status = process_terminate(pipeline->observers[i]);
      if (status == CALL_OK) {
        pipeline->stop_requested[i] = true;
      }
      if (error == CALL_OK) {
        error = status;
      }
    }
  }
  return remember_error(pipeline, error);
}

static size_t observation_interests(struct pyxis_pipeline *pipeline,
    struct wait_interest waits[LAUNCH_BATCH_MAX + 1])
{
  size_t count = 0;
  for (size_t i = 0; i < pipeline->result.stage_count; ++i) {
    if (pipeline->observers[i] && !pipeline->result.stage_valid[i]) {
      waits[count++] = (struct wait_interest){pipeline->observers[i], WAIT_COMPLETE, 0};
    }
  }
  if (pipeline->supervision && !pipeline->group_complete) {
    waits[count++] = (struct wait_interest){pipeline->supervision, WAIT_COMPLETE, 0};
  }
  return count;
}

static enum call_status observe(struct pyxis_pipeline *pipeline, bool *progress)
{
  struct wait_interest waits[LAUNCH_BATCH_MAX + 1];
  uint64_t events[LAUNCH_BATCH_MAX + 1] = {0};
  size_t count = observation_interests(pipeline, waits);
  if (!count) {
    return CALL_OK;
  }
  enum call_status status = wait_many(waits, count, 0, events);
  if (status != CALL_OK) {
    return remember_error(pipeline, status);
  }
  enum call_status error = CALL_OK;
  for (size_t i = 0; i < count; ++i) {
    if (!(events[i] & WAIT_COMPLETE)) {
      continue;
    }
    if (waits[i].handle == pipeline->supervision) {
      status = execution_group_wait(pipeline->supervision);
      if (status == CALL_OK) {
        pipeline->group_complete = true;
        *progress = true;
      }
    } else {
      for (size_t stage = 0; stage < pipeline->result.stage_count; ++stage) {
        if (waits[i].handle == pipeline->observers[stage]) {
          status = process_wait(pipeline->observers[stage], &pipeline->result.stages[stage]);
          if (status == CALL_OK) {
            pipeline->result.stage_valid[stage] = true;
            *progress = true;
          }
          break;
        }
      }
    }
    if (error == CALL_OK) {
      error = status;
    }
  }
  return remember_error(pipeline, error);
}

static enum call_status output_failure(struct pyxis_pipeline *pipeline,
    size_t stream, enum call_status status)
{
  if (stream) {
    pipeline->result.stderr_incomplete = true;
  } else {
    pipeline->result.stdout_incomplete = true;
  }
  struct pipeline_output *output = &pipeline->outputs[stream];
  output->pending_size = output->pending_offset = 0;
  remember_error(pipeline, status);
  remember_error(pipeline, close_handle(&output->reader));
  return status;
}

static enum call_status drain_output(struct pyxis_pipeline *pipeline, size_t stream, bool *progress)
{
  struct pipeline_output *output = &pipeline->outputs[stream];
  if (!output->reader && !output->pending_size) {
    return CALL_OK;
  }
  enum call_status status;
  if (!output->pending_size) {
    size_t amount;
    status = pipe_try_read(output->reader, output->pending, sizeof(output->pending), &amount);
    if (status == CALL_WOULD_BLOCK) {
      /* All child writers have retired. An unconfirmed source-copy release
       * can leave a parent writer alive; do not fabricate EOF or wait forever. */
      return stages_retired(pipeline) ?
          output_failure(pipeline, stream, CALL_OUTCOME_UNKNOWN) : CALL_OK;
    }
    if (status != CALL_OK) {
      return output_failure(pipeline, stream, status);
    }
    *progress = true;
    if (!amount) {
      return remember_error(pipeline, close_handle(&output->reader));
    }
    output->pending_size = amount;
  }
  size_t written;
  status = file_write(output->file, output->written,
      output->pending + output->pending_offset,
      output->pending_size - output->pending_offset, &written);
  if (status != CALL_OK) {
    return output_failure(pipeline, stream, status);
  }
  output->written += written;
  output->pending_offset += written;
  *progress = true;
  if (output->pending_offset == output->pending_size) {
    output->pending_offset = output->pending_size = 0;
  }
  if (output->written >= pipeline->limits[stream]) {
    set_stop(pipeline, stream ? PYXIS_CAPTURE_STOP_STDERR_LIMIT : PYXIS_CAPTURE_STOP_STDOUT_LIMIT);
  }
  return CALL_OK;
}

static enum call_status feed_input(struct pyxis_pipeline *pipeline, bool *progress)
{
  if (!pipeline->input_writer) {
    return CALL_OK;
  }
  if (!pipeline->result.input_unsent) {
    *progress = true;
    return close_input(pipeline);
  }
  size_t written;
  enum call_status status = pipe_try_write(pipeline->input_writer,
      pipeline->input + pipeline->result.input_accepted,
      pipeline->result.input_unsent, &written);
  if (status == CALL_WOULD_BLOCK) {
    return CALL_OK;
  }
  if (status == CALL_ENDPOINT_CLOSED) {
    pipeline->result.input_peer_closed = true;
    *progress = true;
    return close_input(pipeline);
  }
  if (status != CALL_OK) {
    pipeline->result.input_uncertain = status == CALL_OUTCOME_UNKNOWN;
    remember_error(pipeline, status);
    close_input(pipeline);
    return status;
  }
  pipeline->result.input_accepted += written;
  pipeline->result.input_unsent -= written;
  *progress = true;
  return !pipeline->result.input_unsent ? close_input(pipeline) : CALL_OK;
}

static enum call_status final_sizes(struct pyxis_pipeline *pipeline)
{
  struct pyxis_capture_output *outputs[] = {
    &pipeline->result.stdout_output, &pipeline->result.stderr_output,
  };
  for (size_t i = 0; i < 2; ++i) {
    if (outputs[i]->valid) {
      continue;
    }
    uint64_t size;
    enum call_status status = file_size(pipeline->outputs[i].file, &size);
    if (status != CALL_OK) {
      return remember_error(pipeline, status);
    }
    *outputs[i] = (struct pyxis_capture_output){
      .valid = true, .size = size, .truncated = size > pipeline->limits[i],
      .returned_size = size < pipeline->limits[i] ? size : pipeline->limits[i],
    };
  }
  return CALL_OK;
}

static enum call_status advance(struct pyxis_pipeline *pipeline, bool *progress)
{
  *progress = false;
  if (pipeline->result.complete) {
    return final_sizes(pipeline);
  }
  if (pipeline->uncertain && !pipeline->supervision) {
    return CALL_OUTCOME_UNKNOWN;
  }
  enum call_status error = CALL_OK;
  if (pipeline->result.stop == PYXIS_CAPTURE_STOP_NONE && !stages_retired(pipeline)) {
    uint64_t now;
    error = clock_now(pipeline->clock, &now);
    if (error != CALL_OK) {
      remember_error(pipeline, error);
    } else if (now >= pipeline->deadline) {
      set_stop(pipeline, PYXIS_CAPTURE_STOP_DEADLINE);
    }
  }
  if (pipeline->result.stop != PYXIS_CAPTURE_STOP_NONE) {
    enum call_status status = request_stop(pipeline);
    if (error == CALL_OK) {
      error = status;
    }
  }
  enum call_status status = observe(pipeline, progress);
  if (error == CALL_OK) {
    error = status;
  }
  for (size_t i = 0; i < 2; ++i) {
    status = drain_output(pipeline, i, progress);
    if (error == CALL_OK) {
      error = status;
    }
  }
  if (pipeline->result.stop != PYXIS_CAPTURE_STOP_NONE) {
    status = request_stop(pipeline);
  } else {
    status = feed_input(pipeline, progress);
  }
  if (error == CALL_OK) {
    error = status;
  }
  if (pipeline->result.stop != PYXIS_CAPTURE_STOP_NONE) {
    status = request_stop(pipeline);
    if (error == CALL_OK) {
      error = status;
    }
  }
  if (stages_retired(pipeline) && (!pipeline->supervision || pipeline->group_complete) &&
      !pipeline->outputs[0].reader && !pipeline->outputs[1].reader &&
      !pipeline->outputs[0].pending_size && !pipeline->outputs[1].pending_size) {
    status = close_input(pipeline);
    pipeline->result.complete = true;
    pipeline->result.cleanup_unconfirmed = false;
    if (error == CALL_OK) {
      error = status;
    }
    status = final_sizes(pipeline);
    if (error == CALL_OK) {
      error = status;
    }
  }
  return error;
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

static enum call_status close_sources(struct pipe_create_reply *sources, size_t count)
{
  enum call_status error = CALL_OK;
  for (size_t i = 0; i < count; ++i) {
    enum call_status status = close_handle(&sources[i].reader);
    if (error == CALL_OK) {
      error = status;
    }
    status = close_handle(&sources[i].writer);
    if (error == CALL_OK) {
      error = status;
    }
  }
  return error;
}

enum call_status pyxis_pipeline_start(handle_t launcher,
    const struct pyxis_pipeline_options *options, struct pyxis_pipeline **pipeline)
{
  if (!pipeline) {
    return CALL_BAD_REQUEST;
  }
  *pipeline = NULL;
  if (!options || !options->stages || !options->stage_count ||
      options->stage_count > LAUNCH_BATCH_MAX || !options->deadline_ns ||
      (options->input_size && !options->input) ||
      (!options->has_input && (options->input || options->input_size))) {
    return CALL_BAD_REQUEST;
  }
  uint64_t root_rights = DIRECTORY_RIGHT_CREATE | DIRECTORY_RIGHT_REMOVE |
      DIRECTORY_RIGHT_READ_FILES | DIRECTORY_RIGHT_WRITE_FILES;
  enum call_status status = validate_source(launcher, PROTOCOL_LAUNCHER,
      LAUNCHER_RIGHT_LAUNCH);
  if (status == CALL_OK) {
    status = validate_source(options->clock, PROTOCOL_CLOCK, CLOCK_RIGHT_READ);
  }
  if (status == CALL_OK) {
    status = validate_source(options->private_root, PROTOCOL_DIRECTORY, root_rights);
  }
  if (status == CALL_OK) {
    status = validate_source(options->pipe_service, PROTOCOL_PIPE_SERVICE, PIPE_SERVICE_RIGHT_CREATE);
  }
  if (status != CALL_OK) {
    return status;
  }
  struct libc_pipeline_launch *prepared = NULL;
  status = libc_pipeline_prepare(options->stages, options->stage_count,
      options->private_root, options->has_input, &prepared);
  if (status != CALL_OK) {
    return status;
  }
  struct pyxis_pipeline *owned = calloc(1, sizeof(*owned));
  if (!owned) {
    libc_pipeline_discard(prepared);
    return CALL_NO_MEMORY;
  }
  owned->deadline = options->deadline_ns;
  owned->limits[0] = options->stdout_limit ? options->stdout_limit : PYXIS_CAPTURE_DEFAULT_LIMIT;
  owned->limits[1] = options->stderr_limit ? options->stderr_limit : PYXIS_CAPTURE_DEFAULT_LIMIT;
  owned->result.stage_count = options->stage_count;
  owned->result.failed_stage = LAUNCH_NO_STAGE;
  owned->result.has_input = options->has_input;
  owned->result.input_closed = !options->has_input;
  owned->result.input_size = owned->result.input_unsent = options->input_size;
  if (options->input_size) {
    owned->input = malloc(options->input_size);
    if (!owned->input) {
      status = CALL_NO_MEMORY;
    } else {
      memcpy(owned->input, options->input, options->input_size);
    }
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(options->clock, CLOCK_RIGHT_READ, 0, &owned->clock);
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(options->private_root, root_rights, 0, &owned->private_root);
  }
  if (status == CALL_OK) {
    status = handle_copy_restricted(options->pipe_service, PIPE_SERVICE_RIGHT_CREATE, 0, &owned->pipe_service);
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
  for (size_t i = 0; i < 2 && status == CALL_OK; ++i) {
    status = libc_capture_file_create_native(owned->private_root, owned->clock, &owned->outputs[i].file);
  }
  struct pipe_create_reply sources[LAUNCH_BATCH_MAX + 2] = {0};
  size_t middle_count = options->stage_count - 1;
  size_t source_count = middle_count + 2 + options->has_input;
  for (size_t i = 0; i < source_count && status == CALL_OK; ++i) {
    status = pipe_create(owned->pipe_service, &sources[i]);
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
  bool admitted = false;
  if (status == CALL_OK) {
    status = clock_now(owned->clock, &now);
    if (status == CALL_OK && now >= owned->deadline) {
      status = CALL_TIMED_OUT;
    }
  }
  if (status == CALL_OK) {
    handle_t streams[LAUNCH_BATCH_MAX][ENVIRONMENT_STREAM_COUNT] = {0};
    for (size_t i = 0; i < options->stage_count; ++i) {
      streams[i][ENVIRONMENT_STDIN] = i ? sources[i - 1].reader :
          options->has_input ? sources[middle_count + 2].reader : HANDLE_INVALID;
      streams[i][ENVIRONMENT_STDOUT] = sources[i < middle_count ? i : middle_count].writer;
      streams[i][ENVIRONMENT_STDERR] = sources[middle_count + 1].writer;
    }
    status = libc_pipeline_launch(prepared, owned->launcher, streams,
        owned->observers, &owned->result.failed_stage);
    admitted = status == CALL_OK;
    owned->uncertain = status == CALL_OUTCOME_UNKNOWN;
  }
  enum call_status release = libc_pipeline_discard(prepared);
  if (admitted || owned->supervision || owned->uncertain) {
    for (size_t i = 0; i < 2; ++i) {
      owned->outputs[i].reader = sources[middle_count + i].reader;
      sources[middle_count + i].reader = HANDLE_INVALID;
    }
    if (options->has_input) {
      owned->input_writer = sources[middle_count + 2].writer;
      sources[middle_count + 2].writer = HANDLE_INVALID;
    }
  }
  enum call_status closed = close_sources(sources, source_count);
  if (release == CALL_OK) {
    release = closed;
  }
  owned->result.start_status = status;
  if (!admitted && !owned->supervision && !owned->uncertain) {
    discard_pipeline(owned);
    return status;
  }
  if (status != CALL_OK) {
    remember_error(owned, status);
    owned->result.cleanup_unconfirmed = owned->uncertain;
    request_stop(owned);
  } else if (owned->supervision) {
    status = execution_group_seal(owned->supervision);
    if (status != CALL_OK) {
      remember_error(owned, status);
      request_stop(owned);
    }
  }
  if (release != CALL_OK) {
    remember_error(owned, release);
    request_stop(owned);
    if (status == CALL_OK) {
      status = release;
    }
  }
  *pipeline = owned;
  if (status == CALL_OK) {
    bool progress;
    status = advance(owned, &progress);
  }
  return status;
}

enum call_status pyxis_pipeline_step(struct pyxis_pipeline *pipeline,
    struct pyxis_pipeline_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct pyxis_pipeline_result){0};
  if (!pipeline) {
    return CALL_BAD_REQUEST;
  }
  bool progress;
  enum call_status status = advance(pipeline, &progress);
  *result = pipeline->result;
  return status;
}

enum call_status pyxis_pipeline_wait(struct pyxis_pipeline *pipeline,
    struct pyxis_pipeline_result *result)
{
  if (!result || !pipeline) {
    if (result) {
      *result = (struct pyxis_pipeline_result){0};
    }
    return CALL_BAD_REQUEST;
  }
  for (;;) {
    bool progress;
    enum call_status status = advance(pipeline, &progress);
    *result = pipeline->result;
    if (status != CALL_OK || result->complete) {
      return status;
    }
    if (progress) {
      continue;
    }
    struct wait_interest waits[LAUNCH_BATCH_MAX + 4];
    uint64_t events[LAUNCH_BATCH_MAX + 4];
    size_t count = observation_interests(pipeline, waits);
    for (size_t i = 0; i < 2; ++i) {
      if (pipeline->outputs[i].reader) {
        waits[count++] = (struct wait_interest){pipeline->outputs[i].reader, WAIT_READABLE, 0};
      }
    }
    if (pipeline->input_writer) {
      waits[count++] = (struct wait_interest){pipeline->input_writer, WAIT_WRITABLE, 0};
    }
    uint64_t now;
    status = clock_now(pipeline->clock, &now);
    if (status == CALL_OK) {
      uint64_t until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
      if (pipeline->result.stop == PYXIS_CAPTURE_STOP_NONE && !stages_retired(pipeline) &&
          pipeline->deadline < until) {
        until = pipeline->deadline;
      }
      status = count ? wait_many(waits, count, until, events) : CALL_UNAVAILABLE;
    }
    if (status != CALL_OK && status != CALL_TIMED_OUT) {
      remember_error(pipeline, status);
      request_stop(pipeline);
      *result = pipeline->result;
      return status;
    }
  }
}

enum call_status pyxis_pipeline_cancel(struct pyxis_pipeline *pipeline)
{
  if (!pipeline) {
    return CALL_BAD_REQUEST;
  }
  if (pipeline->result.complete) {
    return CALL_OK;
  }
  set_stop(pipeline, PYXIS_CAPTURE_STOP_CANCEL);
  return request_stop(pipeline);
}

enum call_status pyxis_pipeline_read(struct pyxis_pipeline *pipeline,
    enum environment_stream_index stream, uint64_t offset, void *bytes,
    size_t capacity, size_t *read)
{
  if (!read) {
    return CALL_BAD_REQUEST;
  }
  *read = 0;
  if (!pipeline || (stream != ENVIRONMENT_STDOUT && stream != ENVIRONMENT_STDERR) ||
      (capacity && !bytes)) {
    return CALL_BAD_REQUEST;
  }
  if (!pipeline->result.complete) {
    return CALL_BUSY;
  }
  struct pyxis_capture_output *output = stream == ENVIRONMENT_STDOUT ?
      &pipeline->result.stdout_output : &pipeline->result.stderr_output;
  if (!output->valid) {
    return CALL_UNAVAILABLE;
  }
  if (offset >= output->returned_size) {
    capacity = 0;
  } else if (capacity > output->returned_size - offset) {
    capacity = output->returned_size - offset;
  }
  enum call_status status = file_read(pipeline->outputs[stream - ENVIRONMENT_STDOUT].file,
      offset, bytes, capacity, read);
  return status == CALL_OK && capacity && !*read ? CALL_IO : status;
}

enum call_status pyxis_pipeline_close(struct pyxis_pipeline **pipeline)
{
  if (!pipeline) {
    return CALL_BAD_REQUEST;
  }
  if (!*pipeline) {
    return CALL_OK;
  }
  if (!(*pipeline)->result.complete) {
    return CALL_BUSY;
  }
  struct pyxis_pipeline *owned = *pipeline;
  *pipeline = NULL;
  return discard_pipeline(owned);
}
