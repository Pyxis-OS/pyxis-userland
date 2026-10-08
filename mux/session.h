#ifndef MUX_SESSION_H
#define MUX_SESSION_H

#include <abi/process.h>
#include <terminal.h>

struct mux_session {
  handle_t attachment;
  handle_t group;
  handle_t process;
};

/* Borrow startup authority; success returns owned attachment and observers.
 * TAB_WIDTH is 1..32. Failure terminates any created group and releases all
 * owned handles. */
enum call_status mux_session_start(size_t columns, size_t rows, unsigned tab_width,
    struct mux_session *session);

/* Request termination and release ownership without waiting for cleanup. */
void mux_session_release(struct mux_session *session);

#endif
