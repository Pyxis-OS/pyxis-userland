#ifndef USERSPACE_SHELL_CAPTURE_H
#define USERSPACE_SHELL_CAPTURE_H

#include "shell.h"
#include <environment.h>

/* Owned evaluation state. Moving value into an expansion clears that field;
 * observation/control remain explicit independent companions. A service observer
 * is retained until publication; unpublished cleanup must terminate and wait. */
struct shell_capture_result {
  struct environment_value value, observation, control;
  handle_t service;
  enum command_result command;
  struct shell_outcome outcome;
  bool evaluated, published;
};

/* Failure releases owned values but preserves a fault/interruption outcome. */
enum call_status shell_capture_evaluate(struct shell *shell, const char *source,
    const char *destination, struct shell_capture_result *result);
bool shell_capture_close(struct shell_capture_result *result);
/* Preflight a named destination and its potential companion admission before
 * evaluation. This acquires no authority and never mutates the table. */
enum call_status shell_capture_destination(struct shell *shell, const char *name,
    bool management);

#endif
