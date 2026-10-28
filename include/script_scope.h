#ifndef USERSPACE_SCRIPT_SCOPE_H
#define USERSPACE_SCRIPT_SCOPE_H

#include <abi/execution_group.h>
#include <abi/launcher.h>
#include <abi/syscall.h>
#include <stdbool.h>

/* A handed script owns a foreground subgroup: background and successor
 * launches use its original permitted launchers. A positional script keeps
 * those launches under its script subgroup too. An enclosing owner may still
 * stop anything it contains; this helper does not authorize detached escape.
 * Sources are borrowed. Success owns GROUP's CONTROL|WAIT and bound launcher;
 * failure preserves GROUP. A handed script without CHILD_LAUNCHER needs no
 * subgroup; a positional script without LAUNCHER returns DENIED. */
enum call_status script_scope_open(bool positional, handle_t launcher,
    handle_t child_launcher, struct execution_group_create_reply *group);

/* Select the launcher used to admit a process, and the launcher it may receive
 * for its own children. COMMAND_LAUNCHER is an owned command scope's bound
 * launcher, or INVALID when no such scope exists. No authority is acquired. */
handle_t script_scope_launch_launcher(bool positional, bool foreground,
    handle_t own_launcher, const struct execution_group_create_reply *group,
    handle_t command_launcher);
handle_t script_scope_child_launcher(bool positional, bool foreground,
    handle_t child_launcher, const struct execution_group_create_reply *group,
    handle_t command_launcher);

struct script_scope_close_result {
  /* Termination/wait result; it takes precedence over a reference-close error. */
  enum call_status status;
  bool close_failed;
};

/* Stop and wait for the owned subgroup, then release both references even
 * after a cleanup failure. Clear GROUP so a later cleanup cannot replay it.
 * A scope is fully closed only when status is OK and close_failed is false.
 * Closing the final CONTROL on caller death requests the same subtree stop. */
struct script_scope_close_result script_scope_close(
    struct execution_group_create_reply *group);

#endif
