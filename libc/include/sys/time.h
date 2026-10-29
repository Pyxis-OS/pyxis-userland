#ifndef LIBC_SYS_TIME_H
#define LIBC_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* select is declared in sys/select.h; interval timers are not provided. */
struct timeval {
  time_t tv_sec;
  long tv_usec; /* Microseconds, 0..999999. */
};

/* CLOCK_REALTIME with its existing READ|PRECISE authority and UTC accuracy
 * limits. Fractions truncate to microseconds, including before the Unix epoch.
 * Return zero on success, -1 with errno and result unchanged on failure.
 * NULL result fails EINVAL; timezone must be NULL, otherwise ENOTSUP. */
int gettimeofday(struct timeval *__restrict result, void *__restrict timezone);

#ifdef __cplusplus
}
#endif

#endif
