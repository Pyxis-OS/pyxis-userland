#include <environment.h>
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
  /* Explicit selected entries; caller captures and stages its chosen values.
   * Descriptor selection overrides stdin/stdout/stderr and stdin.status only
   * for this child. No implicit environment inheritance. */
  const struct launch_environment_entry *entries;
  size_t entry_count;
  const struct pyxis_working_snapshot *working;
  /* Live descriptors, stdin/stdout/stderr order. -1 deliberately omits a
   * stream. FILE cursors and private input read-ahead are not transferred. */
  int streams[ENVIRONMENT_STREAM_COUNT];
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
 * audits provider behavior throughout the job; copying does not
 * freeze directories or image bytes. CALL_OUTCOME_UNKNOWN must
 * never be retried automatically. Failure clears *job. Single-owner API. */
enum call_status pyxis_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, struct pyxis_job **job);

/* The same explicit native leaf profile and preparation, in a fresh root or
 * subgroup selected by the launcher binding. The creator stays outside and
 * retains sole, non-forwarded CONTROL. No launcher is added to child grants.
 * Preflight and selected-output flushing precede group creation. After one
 * launch, seal subtree admission; caller exit/fault closes CONTROL and stops
 * the subtree. Sources survive; no provider sandbox is implied.
 *
 * Failure may return an owned record when scope cleanup remains: terminate,
 * wait, then close it, never replay admission. Unknown construction without
 * usable supervision quarantines the record until caller teardown. A known
 * scope can confirm cleanup after uncertain leaf admission, but cannot invent
 * a process result. Do not discard a returned record because status failed. */
enum call_status pyxis_job_launch_contained(handle_t launcher,
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
 * does not stop execution. The job retains its observer through retirement.
 * Readiness is process-only; contained wait still confirms subtree cleanup.
 * A failed contained start without a process observer returns HANDLE_INVALID. */
handle_t pyxis_job_observer(const struct pyxis_job *job);

/* Request this process's safe stop, or the contained job's whole subtree,
 * including its threads, without waiting.
 * Idempotent; it never replaces an already committed exit/fault result. */
enum call_status pyxis_job_terminate(struct pyxis_job *job);

/* Observe stage execution-resource retirement and retrieve the actual native
 * result, repeatable. Contained jobs also wait for owned-group completion and
 * attributed cleanup; direct jobs keep process-only retirement. Failure clears
 * result and retains ownership; retirement may wait indefinitely. Cleanup-only
 * completion without a trustworthy process result returns its original start
 * or result-lookup error with a zero result. Confirmed cleanup still permits
 * close; success never invents a process result. Constructor uncertainty stays
 * quarantined. */
enum call_status pyxis_job_wait(struct pyxis_job *job, struct process_result *result);

/* BUSY until confirmed retirement/contained cleanup: never silently detach,
 * stop or block on live execution. Normal cancel/error paths must terminate
 * then wait before close.
 * After retirement, close once and clear *job even on an uncertain release;
 * never retry that release. NULL *job is safe. Direct jobs retain only their
 * existing caller/session fallback; contained jobs retain sole CONTROL until
 * cleanup. Quarantined constructor uncertainty cannot be normally closed. */
enum call_status pyxis_job_close(struct pyxis_job **job);

#ifdef __cplusplus
}
#endif

#endif
