#ifndef LIBC_SYS_SELECT_H
#define LIBC_SYS_SELECT_H

#include <signal.h>
#include <string.h>
#include <sys/time.h>

#define FD_SETSIZE 1024

typedef struct {
  unsigned long __bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

static inline void __pyxis_fd_zero(fd_set *set)
{
  memset(set, 0, sizeof(*set));
}

static inline void __pyxis_fd_set(int descriptor, fd_set *set)
{
  set->__bits[(unsigned)descriptor / (8 * sizeof(unsigned long))] |=
      1UL << ((unsigned)descriptor % (8 * sizeof(unsigned long)));
}

static inline void __pyxis_fd_clear(int descriptor, fd_set *set)
{
  set->__bits[(unsigned)descriptor / (8 * sizeof(unsigned long))] &=
      ~(1UL << ((unsigned)descriptor % (8 * sizeof(unsigned long))));
}

static inline int __pyxis_fd_isset(int descriptor, const fd_set *set)
{
  return (set->__bits[(unsigned)descriptor / (8 * sizeof(unsigned long))] &
      (1UL << ((unsigned)descriptor % (8 * sizeof(unsigned long))))) != 0;
}

#define FD_ZERO(set) __pyxis_fd_zero(set)
#define FD_SET(descriptor, set) __pyxis_fd_set(descriptor, set)
#define FD_CLR(descriptor, set) __pyxis_fd_clear(descriptor, set)
#define FD_ISSET(descriptor, set) __pyxis_fd_isset(descriptor, set)

#ifdef __cplusplus
extern "C" {
#endif

/* The poll ownership/resource contract applies. nfds is highest fd + 1 and
 * must be 0..FD_SETSIZE; FD_* operands must be in that representation's range.
 * Populated exceptfds fail ENOTSUP; invalid selected fds fail EBADF. Sets and
 * timeout remain unchanged on failure. Success replaces sets with ready bits
 * and returns their total across sets. Timeout is never modified. */
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    struct timeval *timeout);
/* As ppoll, sigmask is a real temporary initial-thread mask, restored on all
 * return paths. Worker non-NULL masks fail ENOTSUP. Caught wait signals return
 * EINTR regardless of SA_RESTART, preserving the caller's sets. */
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
    const struct timespec *timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif
