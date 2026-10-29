#ifndef USERSPACE_BUNDLE_LAUNCH_H
#define USERSPACE_BUNDLE_LAUNCH_H

#include <bundle.h>
#include <launcher.h>

struct job_work;
struct bundle_launch;
struct bundle_job_context;

/* Only trusted configuration and successor handoff create these bindings.
 * A manifest selects the suffix; it never adds a source to this inventory. */
#define BUNDLE_SOURCE_PREFIX "source."
#define BUNDLE_SOURCE_LIMIT 8
/* Trusted launcher source; not a URI root or implicitly inherited resource. */
#define BUNDLE_TCP_LISTEN_SOURCE "tcp_listen"

/* Zero-initialize; keep this owner alive through all preparation and close. */
struct bundle_jobs {
  struct bundle_job_context *contexts;
};

struct bundle_authority {
  /* Ordinary class name, or the selected external source key. */
  const char *resource;
  handle_t source;
  uint64_t protocol;
};

/* Borrow configured named sources from an owned immutable capture; preparation queries
 * and attenuates actual handle rights.
 * Keep the capture live through admission; source names/handles borrow it. */
enum call_status bundle_sources_from_capture(const struct environment_capture *capture,
    struct bundle_authority *authority,
    size_t capacity, size_t *count);
/* Trusted successor or exact held-interpreter context handoff; ordinary programs
 * receive sources solely through manifest preparation. Counts append to the caller's arrays. */
enum call_status bundle_sources_forward_capture(const struct environment_capture *capture,
    struct launch_grant *grants,
    size_t grant_capacity, size_t *grant_count, struct launch_environment_entry *entries,
    size_t entry_capacity, size_t *entry_count);

/* The caller selects ordinary authority available to the program; a shell must
 * supply its delegated child launcher, never its own supervision launcher.
 * Preserve explicit stdio/cwd/text/provider and directory entries,
 * replacing app with this bundle's view. Named resources come only from the
 * manifest: unused source resource grants are not installed in the child.
 * Explicit service/create requests use only the supplied held constructor and
 * install it as a private runtime role, without implicit child inheritance.
 * Explicit TCP LISTEN requests select BUNDLE_TCP_LISTEN_SOURCE when supplied,
 * otherwise the caller's declared tcp grant; CONNECT-only requests never use
 * that source. LISTEN grants are private and must be declared again by children.
 * A bundle declaring provider grants receives only those provider roots,
 * selected by source name from held exported PROVIDER entries and attenuated
 * to the declared rights. Other bundles keep existing provider inheritance.
 * Declared jit/manage receives a private native JIT role, and declared
 * process_view/list a private system process view; ordinary children never
 * inherit either. Exact held shell/Lua contexts and trusted session successors
 * retain their source for later declared child launches.
 * Required requests fail as a whole; unavailable optional requests are absent.
 * Both program and source storage remain borrowed until launch is closed.
 * Failure clears *prepared and preserves all source capabilities. */
enum call_status bundle_launch_prepare(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, struct bundle_launch **prepared);

/* A trusted launcher supplies a private provisioning tree separately from
 * ordinary resource inventory. Declared job_work and job_provisioner roles get
 * distinct fresh trees; existing sources in ordinary context are never copied.
 * ALLOW_PROVISIONER gates delegation alone; client leaves remain available.
 * JOBS owns client leaves beyond transient preparation. Admitted delegation
 * trees remain linked for enclosing-owner cleanup or reboot, even after the
 * interpreter PROCESS retires.
 * When a required grant cannot be supplied, absent or with too few rights, the
 * result is CALL_DENIED and MISSING (optional) borrows its manifest name from
 * PROGRAM; otherwise MISSING is NULL. */
enum call_status bundle_launch_prepare_jobs(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, bool allow_provisioner,
    struct bundle_jobs *jobs,
    struct bundle_launch **prepared, const char **missing);
/* Call once after the admission attempt, before releasing preparation. Unknown
 * admission pins the context; never invent retirement or replay admission. */
void bundle_launch_admitted(struct bundle_launch *prepared, enum call_status status,
    handle_t process);
/* Take an admitted delegation for an enclosing owned scope. The caller closes
 * it with job_work_close, removing it only after confirmed scope retirement. */
struct job_work *bundle_launch_take_delegation(struct bundle_launch *prepared);
/* Poll confirmed client retirement; clean only with a valid owned observer.
 * Report otherwise-unobserved startup failures when requested. A caller that
 * already reports its foreground process results passes false to avoid repeats. */
enum call_status bundle_jobs_reap(struct bundle_jobs *jobs, bool report_start_failures);
enum call_status bundle_jobs_close(struct bundle_jobs *jobs);

const struct launch_request *bundle_launch_request(const struct bundle_launch *prepared);
/* Releases preparation storage and unused private context. Admitted context
 * ownership transfers to JOBS until confirmed retirement or owner close. */
void bundle_launch_close(struct bundle_launch *prepared);

#endif
