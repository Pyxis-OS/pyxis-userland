#ifndef MUX_SESSION_H
#define MUX_SESSION_H

#include <abi/process.h>
#include <terminal.h>
#include <pyxis/working_path.h>

struct mux_session {
  handle_t attachment;
  handle_t group;
  handle_t process;
  handle_t cwd_bootstrap;
  handle_t cwd_caller;
};

/* Borrow startup authority and optional cwd through synchronous launch capture;
 * NULL cwd uses the mux's space-start context. Success owns the attachment,
 * observers and private root-shell bootstrap/query channel.
 * A remote broker retains independent cleanup ownership. OUTCOME_UNKNOWN
 * requires mux exit; an uncertain publication cannot be retried.
 * TAB_WIDTH is 1..32. Failure terminates any created group and releases all
 * owned handles. */
enum call_status mux_session_start(size_t columns, size_t rows, unsigned tab_width,
    const struct pyxis_working_snapshot *cwd, struct mux_session *session);

/* Request termination and release ownership without waiting for cleanup. */
void mux_session_release(struct mux_session *session);

#endif
