#include <pyxis/job.h>
#include <stdlib.h>
#include <syscall.h>
#include "job_internal.h"

struct pyxis_job {
  handle_t observer;
  struct process_result result;
  bool complete;
};

static enum call_status launch_job(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, struct pyxis_job **job)
{
  if (!job) {
    return CALL_BAD_REQUEST;
  }
  *job = NULL;
  if (!options) {
    return CALL_BAD_REQUEST;
  }
  struct pyxis_job *owned = calloc(1, sizeof(*owned));
  if (!owned) {
    return CALL_NO_MEMORY;
  }
  enum call_status status = libc_job_launch(launcher, options, private_root, &owned->observer);
  if (status != CALL_OK) {
    free(owned);
    return status;
  }
  *job = owned;
  return CALL_OK;
}

enum call_status pyxis_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, struct pyxis_job **job)
{
  return launch_job(launcher, options, HANDLE_INVALID, job);
}

enum call_status pyxis_job_launch_observer(handle_t launcher,
    const struct pyxis_job_options *options, handle_t *observer)
{
  if (!observer) {
    return CALL_BAD_REQUEST;
  }
  *observer = HANDLE_INVALID;
  if (!options) {
    return CALL_BAD_REQUEST;
  }
  return libc_job_launch(launcher, options, HANDLE_INVALID, observer);
}

enum call_status libc_job_launch_captured(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, struct pyxis_job **job)
{
  if (private_root == HANDLE_INVALID) {
    if (job) {
      *job = NULL;
    }
    return CALL_BAD_REQUEST;
  }
  return launch_job(launcher, options, private_root, job);
}

handle_t pyxis_job_observer(const struct pyxis_job *job)
{
  return job ? job->observer : HANDLE_INVALID;
}

enum call_status pyxis_job_terminate(struct pyxis_job *job)
{
  return job ? process_terminate(job->observer) : CALL_BAD_REQUEST;
}

enum call_status pyxis_job_wait(struct pyxis_job *job, struct process_result *result)
{
  if (!result) {
    return CALL_BAD_REQUEST;
  }
  *result = (struct process_result){0};
  if (!job) {
    return CALL_BAD_REQUEST;
  }
  if (!job->complete) {
    enum call_status status = process_wait(job->observer, &job->result);
    if (status != CALL_OK) {
      return status;
    }
    job->complete = true;
  }
  *result = job->result;
  return CALL_OK;
}

enum call_status pyxis_job_close(struct pyxis_job **job)
{
  if (!job) {
    return CALL_BAD_REQUEST;
  }
  if (!*job) {
    return CALL_OK;
  }
  if (!(*job)->complete) {
    return CALL_BUSY;
  }
  struct syscall_result closed = syscall_close((*job)->observer);
  free(*job);
  *job = NULL;
  if (closed.status >= CALL_STATUS_COUNT || closed.reply_size) {
    return CALL_OUTCOME_UNKNOWN;
  }
  return closed.status;
}
