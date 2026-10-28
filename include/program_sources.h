#ifndef USERSPACE_PROGRAM_SOURCES_H
#define USERSPACE_PROGRAM_SOURCES_H

#include <bundle_launch.h>

/* Own only launch sources, never an entire long-lived startup environment.
 * Authority resource names and handles borrow CAPs in capture. Zero initialize
 * before capture and close once after all borrowing preparation has ended. */
struct program_sources {
  struct environment_capture capture;
  struct bundle_authority *authority;
  size_t count;
  /* Only this prefix may be staged as ordinary continuation resources;
   * the remaining configured entries belong to manifest source selection. */
  size_t ordinary_count;
};

enum call_status program_sources_capture(handle_t environment, struct program_sources *sources);
void program_sources_close(struct program_sources *sources);

/* Borrow canonical manifest resources, runtime source aliases and configured
 * source.* entries from one capture. Configured resource names omit source.;
 * names and handles remain borrowed. No source authority is synthesized.
 * Failure leaves count zero; storage contents may have been written. */
enum call_status program_sources_from_capture(const struct environment_capture *capture,
    struct bundle_authority *authority, size_t capacity, size_t *count);

/* Stage exact continuation sources into caller-owned launch arrays. Existing
 * CAP entries are updated; TEXT collisions refuse. Query actual rights and
 * transport, and validate the declared protocol. No ownership is transferred.
 * The caller still checks origin/publication identity and applies any input
 * attenuation. Pass the ordinary prefix only. Configured source.* bindings
 * must be forwarded separately with bundle_sources_forward_capture, not
 * renamed to ordinary resources here.
 * On failure staging may be partial: do not admit that request. */
enum call_status program_sources_stage(const struct bundle_authority *authority, size_t count,
    struct launch_request *request, struct launch_grant *grants, size_t grant_capacity,
    struct launch_environment_entry *entries, size_t entry_capacity);

#endif
