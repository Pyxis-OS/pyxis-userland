#ifndef LIBC_JOB_INTERNAL_H
#define LIBC_JOB_INTERNAL_H

#include <launcher.h>

struct pyxis_job_options;
struct pyxis_job;
enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, handle_t *observer);
/* The capture owner retains private_root and supplies absent stdin and its
 * private FILE outputs. Uncertain publication clears *job; its outer owner
 * must retain storage until cleanup is confirmed or quarantine it. */
enum call_status libc_job_launch_captured(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, struct pyxis_job **job);

/* First-profile final inventory check, including unnamed grants. This cannot
 * audit mutable namespace bindings or effects brokered by trusted providers. */
enum call_status libc_job_validate_grants(const struct launch_grant *grants, size_t count);
enum call_status libc_job_validate_capture_grants(const struct launch_grant *grants,
    size_t count, handle_t private_root);

#endif
