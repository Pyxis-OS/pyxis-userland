#include <environment.h>
#ifndef LIBC_PYXIS_CAPTURE_H
#define LIBC_PYXIS_CAPTURE_H

#include <pyxis/job.h>
#include <output.h>

#define PYXIS_CAPTURE_DEFAULT_LIMIT (UINT64_C(1024) * 1024)
#define PYXIS_CAPTURE_DEFAULT_POLL_NS UINT64_C(10000000)

struct pyxis_capture;

struct pyxis_capture_options {
  /* Every stream must be -1. This profile supplies its own separate stdout/
   * stderr FILEs and no stdin; all other command inputs remain explicit. */
  struct pyxis_job_options command;
  /* Explicit native pipe CREATE authority for producer/collector construction.
   * Capture retains the process-local collectors and delegates only producers. */
  handle_t output_service;
  handle_t clock;
  uint64_t deadline_ns; /* Required, absolute monotonic; never refreshed. */
  /* Zero selects the defaults above. Limits are per-stream polled triggers
   * and byte-mode read caps, not hard backing quotas; output can overshoot.
   * Handed-off values bypass both policies. Owned take returns full authority. */
  uint64_t stdout_limit, stderr_limit;
  uint64_t poll_interval_ns;
  /* With LAUNCH, an unbound launcher creates a local root for an ungrouped
   * caller; a bound launcher creates a subgroup within its binding. The atomic
   * reply supplies CONTROL|WAIT plus bound LAUNCH. The creator stays outside;
   * the group fixes its space, not CPU. The library retains sole CONTROL,
   * seals subtree admission after direct launch and waits for cleanup;
   * the child receives no descendant launcher. */
  bool fresh_group;
};

enum pyxis_capture_stop {
  PYXIS_CAPTURE_STOP_NONE,
  PYXIS_CAPTURE_STOP_DEADLINE,
  PYXIS_CAPTURE_STOP_STDOUT_LIMIT,
  PYXIS_CAPTURE_STOP_STDERR_LIMIT,
  PYXIS_CAPTURE_STOP_CANCEL,
  PYXIS_CAPTURE_STOP_ERROR,
};

struct pyxis_capture_output {
  bool valid; /* Collected after confirmed retirement. */
  bool taken;
  uint64_t mode; /* OUTPUT_MODE_EMPTY/BYTES/VALUE; sizes apply only to bytes. */
  bool truncated;
  uint64_t size;
  uint64_t returned_size;
};

struct pyxis_capture_result {
  bool complete;
  bool cleanup_unconfirmed;
  bool result_valid;
  enum call_status start_status;
  enum call_status error_status;
  enum pyxis_capture_stop stop;
  struct process_result process;
  struct pyxis_capture_output stdout_output, stderr_output;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Single-owner API. Inputs are borrowed valid C storage until return. Creates
 * both output pairs before launch. Checks the stricter capture inventory,
 * including unnamed/cwd grants; pipe CREATE service grants are refused. The
 * trusted caller audits credentials, subtree/provider and namespace reachability.
 * Synchronous preparation may exceed the deadline; expiry is checked on return.
 *
 * Success returns an owned record. Failure can ALSO return an owned record when
 * admission or group cleanup is pending: keep it and finish cleanup with step/
 * wait/cancel. OUTCOME_UNKNOWN without an observer quarantines direct capture
 * until caller/session teardown: never retry launch or normally close that record.
 * A retained fresh group can confirm cleanup without inventing a process result.
 * Source handles/snapshots survive. No implicit state, bundles or descendants. */
enum call_status pyxis_capture_start(handle_t launcher,
    const struct pyxis_capture_options *options, struct pyxis_capture **capture);

/* Polls readiness and samples the configured size/deadline policy. Native
 * metadata calls may park for worker ownership. No registrations survive.
 * A local stop cause records observation, not proof it caused termination;
 * the actual native result can still be EXITED or FAULTED. First cause wins.
 * Errors preserve ownership and a result snapshot; retry cleanup, never launch. */
enum call_status pyxis_capture_step(struct pyxis_capture *capture,
    struct pyxis_capture_result *result);

/* Uses bounded wait_many intervals with the same absolute deadline. After stop,
 * actual retirement has no wall-time bound (e.g. published HOST work). Native
 * operation failure returns with the record retained for further cleanup. */
enum call_status pyxis_capture_wait(struct pyxis_capture *capture,
    struct pyxis_capture_result *result);

/* Request stop, without waiting; subsequent step/wait observes actual retirement.
 * Idempotent. A committed native exit/fault stays unchanged. */
enum call_status pyxis_capture_cancel(struct pyxis_capture *capture);

/* Confirmed retirement only. STDOUT or STDERR, explicit offsets. Byte-mode reads
 * are capped by interim policy; handed-off FILE values are never capped.
 * No chronology between streams. One native read may be short; zero at the cap
 * is EOF. A non-FILE handed-off value reports its native read/type failure. */
enum call_status pyxis_capture_read(struct pyxis_capture *capture,
    enum environment_stream_index stream, uint64_t offset, void *bytes,
    size_t capacity, size_t *read);

/* Move collected owned primary + optional observation exactly once. Never
 * copies or caps a handed-off value. Byte results expose the complete backing;
 * interim limits are polled stop/read policy, not retention accounting. Failure
 * preserves the record and output. After success read/take is unavailable. */
enum call_status pyxis_capture_take(struct pyxis_capture *capture,
    enum environment_stream_index stream, struct output_take_reply *owned);

/* BUSY until confirmed process/group retirement, permanently for quarantined
 * direct uncertainty. After retirement releases each owned grant once, clears
 * *capture even on uncertain release, and never retries CLOSE. Empty is safe. */
enum call_status pyxis_capture_close(struct pyxis_capture **capture);

#ifdef __cplusplus
}
#endif

#endif
