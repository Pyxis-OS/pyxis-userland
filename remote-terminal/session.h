#ifndef REMOTE_SESSION_H
#define REMOTE_SESSION_H

#include <abi/process.h>
#include <terminal.h>

struct remote_shell {
  handle_t attachment;
  handle_t group;
  handle_t process;
};

/* No echo selects the root shell's quiet line editor; children never inherit it. */
enum call_status remote_shell_launch(size_t columns, size_t rows, unsigned tab_width,
    bool no_echo, struct remote_shell *shell);

#endif
