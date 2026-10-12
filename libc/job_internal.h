#ifndef LIBC_JOB_INTERNAL_H
#define LIBC_JOB_INTERNAL_H

#include <launcher.h>

struct pyxis_job_options;
enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t *observer);

/* First-profile final inventory check, including unnamed grants. This cannot
 * audit mutable namespace bindings or effects brokered by trusted providers. */
enum call_status libc_job_validate_grants(const struct launch_grant *grants, size_t count);

#endif
