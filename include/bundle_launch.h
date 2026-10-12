#ifndef USERSPACE_BUNDLE_LAUNCH_H
#define USERSPACE_BUNDLE_LAUNCH_H

#include <bundle.h>
#include <launcher.h>

struct bundle_launch;
struct bundle_job_context;

/* Zero-initialize; keep this owner alive through all preparation and close. */
struct bundle_jobs {
  struct bundle_job_context *contexts;
};

struct bundle_authority {
  const char *resource;
  handle_t source;
};

/* The caller selects ordinary authority available to the program; a shell must
 * supply its delegated child launcher, never its own supervision launcher.
 * Preserve explicit streams/cwd/environment/namespace and directory roots,
 * replacing app with this bundle's view. Named resources come only from the
 * manifest: unused source resource grants are not installed in the child.
 * Required requests fail as a whole; unavailable optional requests are absent.
 * Both program and source storage remain borrowed until launch is closed.
 * Failure clears *prepared and preserves all source capabilities. */
enum call_status bundle_launch_prepare(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, struct bundle_launch **prepared);

/* A trusted launcher supplies a private provisioning tree separately from
 * ordinary resource inventory. Client job_work sources in that inventory are
 * never forwarded. JOBS owns contexts beyond transient preparation storage. */
enum call_status bundle_launch_prepare_jobs(const struct bundle_program *program,
    const struct launch_request *source, const struct bundle_authority *authority,
    size_t authority_count, handle_t provisioner, struct bundle_jobs *jobs,
    struct bundle_launch **prepared);
/* Call once after the admission attempt, before releasing preparation. Unknown
 * admission pins the context; never invent retirement or replay admission. */
void bundle_launch_admitted(struct bundle_launch *prepared, enum call_status status,
    handle_t process);
/* Poll confirmed client retirement; clean only with a valid owned observer. */
enum call_status bundle_jobs_reap(struct bundle_jobs *jobs);
enum call_status bundle_jobs_close(struct bundle_jobs *jobs);

const struct launch_request *bundle_launch_request(const struct bundle_launch *prepared);
/* Releases preparation storage and unused private context. Admitted context
 * ownership transfers to JOBS until confirmed retirement or owner close. */
void bundle_launch_close(struct bundle_launch *prepared);

#endif
