#ifndef USERSPACE_SHELL_LAUNCH_CAPTURE_H
#define USERSPACE_SHELL_LAUNCH_CAPTURE_H

#include "shell.h"
#include <forwarder.h>

/* Unpublished grants and a leaf provider observer. After successful ENV
 * publication, close the local copies. Before publication, failure must
 * terminate/wait the provider before releasing its observer and pair. */
struct shell_service_value {
  struct forwarder_reply slot;
  handle_t process;
  bool evaluated;
};

#endif
