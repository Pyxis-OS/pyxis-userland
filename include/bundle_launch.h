#ifndef USERSPACE_BUNDLE_LAUNCH_H
#define USERSPACE_BUNDLE_LAUNCH_H

#include <bundle.h>
#include <launcher.h>

struct bundle_launch;

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

const struct launch_request *bundle_launch_request(const struct bundle_launch *prepared);
/* Releases only preparation storage, never borrowed source handles. */
void bundle_launch_close(struct bundle_launch *prepared);

#endif
