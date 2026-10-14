#include <environment.h>
#ifndef LIBC_PYXIS_PIPELINE_H
#define LIBC_PYXIS_PIPELINE_H

#include <pyxis/capture.h>

struct pyxis_pipeline;

struct pyxis_pipeline_options {
  /* Explicit stages; all supplied streams must be -1. The library installs
   * directional pipes, without inheriting live streams or other grants. */
  const struct pyxis_job_options *stages;
  size_t stage_count;
  /* Trusted, exclusively provisioned RAM root. The capture audit/lifetime
   * prerequisites from capture.h apply to every stage. Sources survive. */
  handle_t private_root, clock, pipe_service;
  /* Absent input requires NULL/zero. Present input is copied before effects;
   * present zero bytes supplies real EOF. No live producer or callback source. */
  bool has_input;
  const void *input;
  size_t input_size;
  uint64_t deadline_ns;
  /* Zero selects capture.h's 1 MiB polled trigger and returned cap. Combined
   * stderr has no stage attribution; intermediate stdout only feeds its peer. */
  uint64_t stdout_limit, stderr_limit;
  /* With LAUNCH, an unbound launcher creates a local root for an ungrouped
   * caller; a bound launcher creates a subgroup within its binding. The atomic
   * reply supplies CONTROL|WAIT plus bound LAUNCH. The creator stays outside;
   * the group fixes its space, not CPU. The library retains sole CONTROL,
   * seals subtree admission after direct batch launch and waits for cleanup;
   * stages receive no descendant launcher. */
  bool fresh_group;
};

struct pyxis_pipeline_result {
  bool complete;
  bool cleanup_unconfirmed;
  enum call_status start_status, error_status;
  uint64_t failed_stage; /* LAUNCH_NO_STAGE or a refused batch stage. */
  enum pyxis_capture_stop stop;
  size_t stage_count;
  bool stage_valid[LAUNCH_BATCH_MAX];
  struct process_result stages[LAUNCH_BATCH_MAX];
  bool has_input, input_closed, input_peer_closed, input_uncertain;
  uint64_t input_size, input_accepted, input_unsent;
  struct pyxis_capture_output stdout_output, stderr_output;
  /* A failed output transfer can discard its queued tail. Retained bytes are
   * readable after cleanup; neither EOF nor a complete capture is invented. */
  bool stdout_incomplete, stderr_incomplete;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Single owner; valid borrowed C inputs and separate output until return.
 * Preflight all metadata/attachments and own the input before effectful setup,
 * then publish exactly one batch. Preserves stage sources/snapshots. Trusted
 * caller audits private-tree, credential and provider reachability.
 *
 * A failure may return an owned record: finish its cleanup, never replay launch.
 * Unknown admission without observers quarantines captures until caller/session
 * teardown. A retained fresh group stops/waits without inventing stage results.
 * Synchronous admission and cleanup have no hard wall-time bound. */
enum call_status pyxis_pipeline_start(handle_t launcher,
    const struct pyxis_pipeline_options *options, struct pyxis_pipeline **pipeline);

/* One bounded round: try input while draining both outputs and observing every
 * stage. Stage exit/fault is a result, not an automatic pump failure. EPIPE
 * closes the feeder and reports confirmed acceptance, not child consumption.
 * Other pump errors stop all stages and preserve ownership for cleanup retries.
 * Progress never refreshes the absolute deadline. */
enum call_status pyxis_pipeline_step(struct pyxis_pipeline *pipeline,
    struct pyxis_pipeline_result *result);

/* Runs concurrent rounds; wait_many only when no immediate progress remains.
 * Drains buffered output through EOF and observes every stage (and the optional
 * fresh group) before completion. After stop, retirement may remain unbounded. */
enum call_status pyxis_pipeline_wait(struct pyxis_pipeline *pipeline,
    struct pyxis_pipeline_result *result);

enum call_status pyxis_pipeline_cancel(struct pyxis_pipeline *pipeline);

/* After confirmed cleanup, explicit STDOUT/STDERR and offset, capped short
 * native reads; no backing capability or chronology between streams. */
enum call_status pyxis_pipeline_read(struct pyxis_pipeline *pipeline,
    enum environment_stream_index stream, uint64_t offset, void *bytes,
    size_t capacity, size_t *read);

/* BUSY until cleanup, permanently for quarantined direct uncertainty. Once
 * complete, release owned grants once and clear *pipeline even on close error. */
enum call_status pyxis_pipeline_close(struct pyxis_pipeline **pipeline);

#ifdef __cplusplus
}
#endif

#endif
