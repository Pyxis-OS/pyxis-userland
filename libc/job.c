#include <pyxis/job.h>
#include <errno.h>
#include <stdlib.h>
#include <syscall.h>
#include "job_internal.h"

struct pyxis_job {
  handle_t observer, supervision;
  struct process_result result;
  enum call_status start_status, result_error;
  bool process_complete, group_complete, stop_requested, quarantined;
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

static bool retired(const struct pyxis_job *job)
{
  /* Subtree completion proves retirement even without a trustworthy exit reply. */
  return !job->quarantined && (job->supervision ? job->group_complete : job->process_complete);
}

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

enum call_status pyxis_job_launch_contained(handle_t launcher,
    const struct pyxis_job_options *options, struct pyxis_job **job)
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
  struct job_preparation prepared = {0};
  enum call_status status = libc_job_prepare(&prepared, launcher, options, HANDLE_INVALID);
  struct execution_group_create_reply group = {0};
  if (status == CALL_OK) {
    status = launcher_create_group(prepared.launcher, &group);
    if (status == CALL_OK) {
      owned->supervision = group.supervision;
    } else if (status == CALL_OUTCOME_UNKNOWN) {
      owned->quarantined = true;
    }
  }
  if (status == CALL_OK) {
    status = libc_job_publish(&prepared, group.launcher, &owned->observer);
  }
  if (status == CALL_OK) {
    status = execution_group_seal(owned->supervision);
  }
  int saved_errno = errno;
  enum call_status release = close_handle(&group.launcher);
  enum call_status discarded = libc_job_discard_preparation(&prepared);
  errno = saved_errno;
  if (status == CALL_OK) {
    status = release != CALL_OK ? release : discarded;
  }
  owned->start_status = status;
  if (status != CALL_OK) {
    if (!owned->supervision && !owned->quarantined) {
      free(owned);
      return status;
    }
    if (owned->supervision) {
      pyxis_job_terminate(owned);
    }
  }
  *job = owned;
  return status;
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
  if (!job) {
    return CALL_BAD_REQUEST;
  }
  if (job->quarantined) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (job->supervision) {
    enum call_status status = execution_group_terminate(job->supervision);
    if (status == CALL_OK) {
      job->stop_requested = true;
    }
    return status;
  }
  return process_terminate(job->observer);
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
  if (job->quarantined) {
    return CALL_OUTCOME_UNKNOWN;
  }
  if (job->supervision && job->start_status != CALL_OK &&
      !job->stop_requested && !job->group_complete) {
    enum call_status status = pyxis_job_terminate(job);
    if (status != CALL_OK) {
      return status;
    }
  }
  if (job->observer && !job->process_complete) {
    enum call_status status = process_wait(job->observer, &job->result);
    if (status == CALL_OK) {
      job->process_complete = true;
    } else if (!job->supervision) {
      return status;
    } else {
      if (job->result_error == CALL_OK) {
        job->result_error = status;
      }
      if (!job->stop_requested && !job->group_complete) {
        status = pyxis_job_terminate(job);
        if (status != CALL_OK) {
          return status;
        }
      }
    }
  }
  if (job->supervision && !job->group_complete) {
    enum call_status status = execution_group_wait(job->supervision);
    if (status != CALL_OK) {
      return status;
    }
    job->group_complete = true;
  }
  if (!job->process_complete) {
    if (job->start_status != CALL_OK) {
      return job->start_status;
    }
    return job->result_error != CALL_OK ? job->result_error : CALL_OUTCOME_UNKNOWN;
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
  if (!retired(*job)) {
    return CALL_BUSY;
  }
  struct pyxis_job *owned = *job;
  *job = NULL;
  enum call_status status = close_handle(&owned->observer);
  enum call_status supervision = close_handle(&owned->supervision);
  free(owned);
  return status != CALL_OK ? status : supervision;
}
