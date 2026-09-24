#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stdint.h>
#include <stddef.h>

/* Unix UTC seconds, without distinct leap-second representation. */
typedef int64_t time_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;  /* January is zero. */
  int tm_year; /* Years since 1900. */
  int tm_wday; /* Sunday is zero. */
  int tm_yday; /* January 1 is zero. */
  int tm_isdst;
};

#define TIME_UTC 1

/* Borrow the named startup clock with READ authority. time returns -1 on
 * failure (also a valid pre-epoch timestamp); errno is preserved on success.
 * timespec_get supports only TIME_UTC, returns it on success, otherwise zero
 * with errno set and destination unchanged. Fractional units do not imply
 * nanosecond accuracy: the boot seed is approximate and no resync exists. */
time_t time(time_t *result);
int timespec_get(struct timespec *result, int base);

/* Proleptic Gregorian UTC conversion, including negative timestamps.
 * Fail with EOVERFLOW if tm_year cannot fit in int, EINVAL for NULL inputs.
 * gmtime_r leaves the destination unchanged on failure; gmtime returns borrowed
 * static storage overwritten by later successful gmtime calls. No allocation.
 * Local time, mktime, strftime and locale selection are not implemented. */
struct tm *gmtime_r(const time_t *restrict timer, struct tm *restrict result);
struct tm *gmtime(const time_t *timer);

#endif
