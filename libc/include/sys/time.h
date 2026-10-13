#ifndef LIBC_SYS_TIME_H
#define LIBC_SYS_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Only the structure. gettimeofday, select and the interval timers are not
 * provided; use timespec_get or clock_gettime. */
struct timeval {
  time_t tv_sec;
  long tv_usec; /* Microseconds, 0..999999. */
};

#ifdef __cplusplus
}
#endif

#endif
