#ifndef LIBC_RUNTIME_H
#define LIBC_RUNTIME_H

#include <abi/process.h>
#include <abi/handle.h>

/* No TLS, heap, stdio or startup-record access, including before FS is bound. */
[[noreturn]] void libc_start_failed(unsigned step, enum call_status status);
void libc_startup_close(handle_t handle, unsigned step);

void malloc_init(void);
void stdio_init(void);
int stdio_finish(void);

/* Itanium C++ ABI entry used by compiler-generated static destructors. */
int __cxa_atexit(void (*handler)(void *), void *argument, void *module);

#endif
