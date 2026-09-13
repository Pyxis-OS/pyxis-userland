#ifndef USERSPACE_EXIT_H
#define USERSPACE_EXIT_H

/* Terminate the current run with a signed 32-bit status. No return to the
 * caller, stream flushing, or user cleanup handlers. */
[[noreturn]] void exit(int status);

#endif
