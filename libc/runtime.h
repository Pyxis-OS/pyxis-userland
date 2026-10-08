#ifndef LIBC_RUNTIME_H
#define LIBC_RUNTIME_H

void malloc_init(void);
void stdio_init(void);
void stdio_finish(void);

/* Itanium C++ ABI entry used by compiler-generated static destructors. */
int __cxa_atexit(void (*handler)(void *), void *argument, void *module);

#endif
