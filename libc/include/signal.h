#ifndef LIBC_SIGNAL_H
#define LIBC_SIGNAL_H

typedef int sig_atomic_t;
typedef struct { unsigned long __opaque; } sigset_t;

union sigval {
  int sival_int;
  void *sival_ptr;
};

typedef struct {
  int si_signo, si_errno, si_code;
  int si_pid;
  unsigned int si_uid;
  int si_status;
  void *si_addr;
  long si_band;
  int si_fd;
  union sigval si_value;
} siginfo_t;

struct sigaction {
  union {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
  };
  sigset_t sa_mask;
  int sa_flags;
};

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGSTKFLT 16
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define SIGIO 29
#define SIGPWR 30
#define SIGSYS 31
#define NSIG 32

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000U

#define SI_USER 0
#define SI_KERNEL 128

#ifdef __cplusplus
extern "C" {
#endif

/* Cooperative WINCH/INT delivery on the initial thread at audited outer libc
 * boundaries. No asynchronous upcalls, fault handlers or compute-loop delivery.
 * Handler installation selects terminal stdout, stderr, then stdin for WINCH;
 * its exact fd generation is retained, without copying native I/O authority.
 * INT handler/ignore opts into the singleton native receiver; conflicts fail
 * EBUSY. DFL detaches libc's receiver. Raw PASSTHROUGH still makes Ctrl+C data.
 * PIPE only permits queryable DFL/IGN and remains EPIPE-only, never delivered.
 * Other declared signals fail ENOTSUP; invalid numbers fail EINVAL.
 *
 * RESTART, RESETHAND, NODEFER and SIGINFO are real; unsupported declared flags
 * fail ENOTSUP, unknown bits EINVAL. SIGINFO supplies signum and SI_USER for
 * raise or SI_KERNEL for native events; other fields are zero, context NULL.
 * Each dispatch uses a bounded pending snapshot. Clean nested calls follow the
 * effective mask; NODEFER omits self-blocking but never an explicit sa_mask bit.
 * Handlers return normally or terminate. Longjmp, C++ exception and thread-exit
 * escapes are unsupported. Initial-thread retirement disables this profile.
 * Fresh native images start with default actions and empty masks/pending state. */
int sigaction(int number, const struct sigaction *__restrict action,
    struct sigaction *__restrict previous);
/* Persistent handler, empty action mask, automatic self-blocking and the
 * supported SA_RESTART read policy. */
void (*signal(int number, void (*handler)(int)))(int);
int raise(int number);

/* Set values can be manipulated in workers. Mask/action/pending/suspend/raise
 * operations are initial-thread-only and fail ENOTSUP in workers. Masks do not
 * intercept native faults or forced termination. Invalid values leave outputs
 * unchanged. NULL set queries the current mask without changing it. */
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int number);
int sigdelset(sigset_t *set, int number);
int sigismember(const sigset_t *set, int number);
int sigprocmask(int how, const sigset_t *__restrict set, sigset_t *__restrict previous);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

#ifdef __cplusplus
}
#endif

#endif
