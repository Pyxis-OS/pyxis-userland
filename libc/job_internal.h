#include <environment.h>
#ifndef LIBC_JOB_INTERNAL_H
#define LIBC_JOB_INTERNAL_H

#include <launcher.h>
#include <pyxis/descriptor.h>

struct pyxis_job_options;
struct pyxis_job;

struct job_stream_selection {
  uint64_t protocol, grant, observation_protocol, observation_grant;
  bool append;
};

struct job_preparation {
  struct launch_request request;
  unsigned char *metadata;
  size_t used;
  enum call_status error;
  struct launch_grant *grants;
  size_t copied_grants;
  struct pyxis_descriptor_binding streams[ENVIRONMENT_STREAM_COUNT];
  struct job_stream_selection selected_streams[ENVIRONMENT_STREAM_COUNT];
  struct launch_environment_entry *entries;
  handle_t image, launcher;
  bool attempted;
};

/* Zero-initialized owner storage; discard even after preparation fails. Capture
 * the existing metadata/authority inventory and flush selected outputs before
 * any contained-scope creation. Publication borrows the selected launcher and
 * may be attempted only once, including after an uncertain result. */
enum call_status libc_job_prepare(struct job_preparation *prepared, handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root);
enum call_status libc_job_publish(struct job_preparation *prepared, handle_t launcher,
    handle_t *observer);
enum call_status libc_job_discard_preparation(struct job_preparation *prepared);

enum call_status libc_job_launch(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, handle_t *observer);
/* The capture owner retains its output account and supplies absent stdin and
 * FILE-compatible producer outputs. Uncertain publication clears *job; its outer owner
 * must retain storage until cleanup is confirmed or quarantine it. */
enum call_status libc_job_launch_captured(handle_t launcher,
    const struct pyxis_job_options *options, handle_t private_root, struct pyxis_job **job);

/* Capture inventory: private_root is a legacy pipeline directory or a native
 * retention account. Capture profiles exclude child retention grants. This cannot
 * audit mutable environment bindings or effects brokered by trusted providers. */
enum call_status libc_job_validate_grants(const struct launch_grant *grants, size_t count);
enum call_status libc_job_validate_capture_grants(const struct launch_grant *grants,
    size_t count, handle_t private_root);

#endif
