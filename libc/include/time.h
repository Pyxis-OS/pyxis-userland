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
  long tm_gmtoff; /* Seconds east of UTC; zero for gmtime. */
  const char *tm_zone; /* Borrowed timezone designation; UTC for gmtime. */
};

#define TIME_UTC 1

/* Borrow the named startup clock with READ authority. time returns -1 on
 * failure (also a valid pre-epoch timestamp); errno is preserved on success.
 * timespec_get supports only TIME_UTC, returns it on success, otherwise zero
 * with errno set and destination unchanged. Fractional units do not imply
 * nanosecond accuracy: the boot seed is approximate and no resync exists. */
time_t time(time_t *result);
int timespec_get(struct timespec *result, int base);
/* Seconds between end and beginning, without overflowing time_t arithmetic. */
double difftime(time_t end, time_t beginning);

/* English C-locale standard C/POSIX conversions, with E/O alternatives equal
 * to their ordinary forms. No locale selection or extended flags/widths.
 * Return the byte count excluding NUL, or zero for insufficient capacity.
 * Invalid arguments/conversions return zero with EINVAL; output on failure is
 * unspecified. %z has the standard minute precision; %Z uses tm_zone, or an
 * empty string if unknown. Neither conversion consults the current TZ. */
size_t strftime(char *restrict output, size_t capacity,
    const char *restrict format, const struct tm *restrict calendar);

/* Proleptic Gregorian UTC conversion, including negative timestamps.
 * Fail with EOVERFLOW if tm_year cannot fit in int, EINVAL for NULL inputs.
 * gmtime_r leaves the destination unchanged on failure; gmtime returns borrowed
 * static storage overwritten by later successful gmtime calls. No allocation.
 * mktime and locale selection are not implemented. */
struct tm *gmtime_r(const time_t *restrict timer, struct tm *restrict result);
struct tm *gmtime(const time_t *timer);

/* TZ absent/empty means UTC without file access. Otherwise TZ is an IANA name
 * under boot://share/zoneinfo, using the process's boot directory capability.
 * Named zones are loaded lazily and cached in process-owned heap storage.
 * Return NULL with errno on file/allocation errors, EINVAL for invalid names or
 * malformed/unsupported TZif, ENOTSUP where zone data leaves time unspecified,
 * and EOVERFLOW when the calendar cannot be represented. No UTC fallback.
 * Success preserves errno; localtime_r leaves result unchanged on failure.
 * localtime returns static storage overwritten by its next successful call.
 * tm_zone points into the cached zone and is valid until a successful selection
 * reload or process exit; UTC uses permanent storage. Startup TZ is immutable.
 * The cache and static results assume one thread per process, like errno. */
struct tm *localtime_r(const time_t *restrict timer, struct tm *restrict result);
struct tm *localtime(const time_t *timer);

#endif
