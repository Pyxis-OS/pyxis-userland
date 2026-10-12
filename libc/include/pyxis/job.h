#ifndef LIBC_PYXIS_JOB_H
#define LIBC_PYXIS_JOB_H

#include <launcher.h>
#include <process.h>
#include <pyxis/environment.h>
#include <pyxis/working_path.h>

struct pyxis_job;

struct pyxis_job_options {
  handle_t image;
  const char *const *argv;
  size_t argc;
  const struct launch_grant *grants;
  size_t grant_count;
  const struct launch_binding *resources;
  size_t resource_count;
  const struct launch_binding *roots;
  size_t root_count;
  /* NULL selects no cwd/empty environment, never ambient inheritance. Only
   * the working chain and spelling are used; roots/namespace below are exact. */
  const struct pyxis_working_snapshot *working;
  const struct pyxis_environment_snapshot *environment;
  handle_t namespace_handle;
  /* Live descriptors, stdin/stdout/stderr order. -1 deliberately omits a
   * stream. FILE cursors and private input read-ahead are not transferred. */
  int streams[STARTUP_STREAM_COUNT];
  uint64_t initial_stack_bytes;
};

#ifdef __cplusplus
extern "C" {
#endif

/* One native image, through the caller's existing launcher. Inputs are borrowed
 * for this call; output must be separate. Snapshots own names, argv, cwd/env
 * and restricted handle copies through synchronous launch capture. Source
 * handles/descriptors survive. Selected buffered outputs flush before launch;
 * a flush failure returns IO and can have committed bytes, with errno retained.
 * No lookup, bundle/shebang expansion, implicit grants or descendant launcher.
 * All final grants are checked, including unnamed entries. The trusted caller
 * audits namespace/provider behavior throughout the job; copying does not
 * freeze namespaces, directories or image bytes. CALL_OUTCOME_UNKNOWN must
 * never be retried automatically. Failure clears *job. Single-owner API. */
enum call_status pyxis_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, struct pyxis_job **job);

/* Same direct launch preparation and authority checks, returning an owned
 * WAIT|TERMINATE observer without a job record. The caller can wait, terminate
 * or close to detach; close never stops the child. Failure clears *observer,
 * including unknown admission, which must not be retried. For event-loop
 * adapters with their own lifecycle, not capturing jobs. */
enum call_status pyxis_job_launch_observer(handle_t launcher,
    const struct pyxis_job_options *options, handle_t *observer);

/* Borrowed WAIT|TERMINATE observer for readiness/queries. Never close it; copy
 * an attenuated WAIT handle to own an independent observer. Closing observers
 * does not stop execution. The job retains its observer through retirement. */
handle_t pyxis_job_observer(const struct pyxis_job *job);

/* Request this process's safe stop, including its threads, without waiting.
 * Idempotent; it never replaces an already committed exit/fault result. */
enum call_status pyxis_job_terminate(struct pyxis_job *job);

/* Observe stage execution-resource retirement and retrieve the actual native
 * result, repeatable. Not group/deferred-object cleanup. Failure clears result
 * and preserves the owned observer; retirement may wait indefinitely. */
enum call_status pyxis_job_wait(struct pyxis_job *job, struct process_result *result);

/* BUSY until successful wait: never silently detach, stop or block on a live
 * child. Normal cancel/error paths must terminate then wait before close.
 * After retirement, close once and clear *job even on an uncertain release;
 * never retry that release. NULL *job is safe. Caller death has only its existing
 * session-group fallback, not a per-job fail-closed guarantee. */
enum call_status pyxis_job_close(struct pyxis_job **job);

#ifdef __cplusplus
}
#endif

#endif
