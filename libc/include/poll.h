#ifndef LIBC_POLL_H
#define LIBC_POLL_H

#include <signal.h>
#include <time.h>

typedef unsigned long nfds_t;

struct pollfd {
  int fd;
  short events;
  short revents;
};

#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200

#ifdef __cplusplus
extern "C" {
#endif

/* Native console/PIPE and positioned FILE descriptors. Responses/transactions
 * fail ENOTSUP; priority/band requests do likewise. Negative fds are ignored.
 * ERR/HUP/NVAL are returned independently of requested events. No I/O is
 * consumed, flushed or reserved. FILE pushback/indicators are not readiness.
 * The caller excludes conflicting I/O, rebuffering, close/replacement and
 * transaction commit through the watched open objects while waiting. Internal
 * storage pins do not prolong native endpoint ownership. Observed invalidation
 * returns NVAL, but close is not a prompt cancellation mechanism.
 * Timeout is milliseconds, negative infinite, zero immediate. Calls that can
 * park need clock READ|PRECISE, not SLEEP. The row bound is INT_MAX plus checked
 * memory/native admission limits; FD_SETSIZE is not a poll bound.
 * Failure leaves every revents unchanged; success returns ready row count. */
int poll(struct pollfd *descriptors, nfds_t count, int timeout);
/* Nanosecond timeout, unchanged on return; NULL means infinite. Signals are
 * absent: sigmask is accepted as a no-op, including non-NULL, and never read.
 * This provides no atomic signal-mask transition or signal-driven EINTR. */
int ppoll(struct pollfd *descriptors, nfds_t count, const struct timespec *timeout,
    const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif
