#ifndef LIBC_JOB_PIPELINE_LAUNCH_H
#define LIBC_JOB_PIPELINE_LAUNCH_H

#include <pyxis/job.h>

struct libc_pipeline_launch;

/* Prepare every stage before effectful capture/pipe setup. Options streams
 * must all be -1. Owns metadata and restricted image/grant copies; no ambient
 * inheritance or image lookup. Retains private_root only for inventory checks.
 * Stdin is reserved for stage zero iff has_input, and for every later stage;
 * all stdout/stderr entries are reserved for directional native PIPE grants. */
enum call_status libc_pipeline_prepare(const struct pyxis_job_options *stages,
    size_t count, handle_t private_root, bool has_input,
    struct libc_pipeline_launch **prepared);

/* Single attempt, including preparation failures. streams supplies borrowed
 * native pipe endpoints for reserved slots, HANDLE_INVALID for absent stdin.
 * All final grants are audited before one atomic batch submission. Output
 * observers always belong to the caller, including after discard failures.
 * CALL_OUTCOME_UNKNOWN must never be replayed. Discard after every attempt. */
enum call_status libc_pipeline_launch(struct libc_pipeline_launch *prepared,
    handle_t launcher, const handle_t streams[][STARTUP_STREAM_COUNT],
    handle_t *observers, uint64_t *failed_index);

/* Consumes the preparation, including on failure. Closes every owned handle
 * once and returns the first release failure. Never retry CLOSE. NULL is safe. */
enum call_status libc_pipeline_discard(struct libc_pipeline_launch *prepared);

#endif
