#ifndef LIBC_SYS_TIME_H
#define LIBC_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* gettimeofday and interval timers are not provided; use clock_gettime.
 * select is declared in sys/select.h. */
struct timeval {
  time_t tv_sec;
  long tv_usec; /* Microseconds, 0..999999. */
};

#ifdef __cplusplus
}
#endif

#endif
