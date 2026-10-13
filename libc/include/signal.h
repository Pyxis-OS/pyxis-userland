#ifndef LIBC_SIGNAL_H
#define LIBC_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pyxis has no signals: nothing is delivered to a handler, and no event raises
 * one. The header exists so ISO C programs compile; signal and raise report
 * failure. Programs that depend on delivery fail at run time, not silently. */
typedef int sig_atomic_t;

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

/* The six ISO C signal numbers, with the values musl uses on x86-64. */
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15

/* Both fail. A number outside the six sets EINVAL; otherwise errno is ENOTSUP.
 * signal returns SIG_ERR and installs nothing; raise returns -1 and runs no
 * handler. abort does not call raise. */
void (*signal(int number, void (*handler)(int)))(int);
int raise(int number);

#ifdef __cplusplus
}
#endif

#endif
