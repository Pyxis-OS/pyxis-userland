#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
#define TIME_MONOTONIC 2

/* Borrow the named startup clock with READ authority. time returns -1 on
 * failure (also a valid pre-epoch timestamp); errno is preserved on success.
 * timespec_get supports TIME_UTC and TIME_MONOTONIC, returns the base on
 * success, otherwise zero with errno set and destination unchanged. UTC
 * fractional units do not imply nanosecond accuracy: the boot seed is
 * approximate and no resync exists. Monotonic time counts from an epoch set
 * during boot and need not include time while a VM is paused or suspended. */
time_t time(time_t *result);
int timespec_get(struct timespec *result, int base);
/* Seconds between end and beginning, without overflowing time_t arithmetic. */
double difftime(time_t end, time_t beginning);

typedef int clockid_t;

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

/* POSIX clocks on the startup clock: CLOCK_REALTIME reads it as timespec_get's
 * TIME_UTC, CLOCK_MONOTONIC as TIME_MONOTONIC. Other clocks fail with EINVAL. */
int clock_gettime(clockid_t clock, struct timespec *result);
/* Sleep on the startup clock, which needs SLEEP authority, until the duration
 * has passed. Pyxis has no signals, so it never returns early with EINTR and
 * never writes remaining. A tv_nsec outside 0..999999999 or a negative tv_sec
 * fails with EINVAL; a duration past the clock's range sleeps until its end. */
int nanosleep(const struct timespec *duration, struct timespec *remaining);

/* English C-locale standard C/POSIX conversions, with E/O alternatives equal
 * to their ordinary forms. No locale selection or extended flags/widths.
 * Return the byte count excluding NUL, or zero for insufficient capacity.
 * Invalid arguments/conversions return zero with EINVAL; output on failure is
 * unspecified. %z has the standard minute precision; %Z uses tm_zone, or an
 * empty string if unknown. Neither conversion consults the current TZ. */
size_t strftime(char *__restrict output, size_t capacity,
    const char *__restrict format, const struct tm *__restrict calendar);

/* Proleptic Gregorian UTC conversion, including negative timestamps.
 * Fail with EOVERFLOW if tm_year cannot fit in int, EINVAL for NULL inputs.
 * gmtime_r leaves the destination unchanged on failure; gmtime returns borrowed
 * static storage overwritten by later successful gmtime calls. No allocation.
 * Locale selection is not implemented. */
struct tm *gmtime_r(const time_t *__restrict timer, struct tm *__restrict result);
struct tm *gmtime(const time_t *timer);

/* TZ absent/empty means UTC without file access. Otherwise TZ is an IANA name
 * under boot://share/zoneinfo, using the process's boot directory capability.
 * TZ is read from the current environment on each call; a changed name loads
 * that zone lazily into process-owned heap storage, replacing the cache.
 * Return NULL with errno on file/allocation errors, EINVAL for invalid names or
 * malformed/unsupported TZif, ENOTSUP where zone data leaves time unspecified,
 * and EOVERFLOW when the calendar cannot be represented. No UTC fallback.
 * Success preserves errno; localtime_r leaves result unchanged on failure.
 * localtime returns static storage overwritten by its next successful call.
 * tm_zone points into the cached zone and is valid until a successful selection
 * reload or process exit; UTC uses permanent storage.
 * The cache and static results assume one thread per process, like errno. */
struct tm *localtime_r(const time_t *__restrict timer, struct tm *__restrict result);
struct tm *localtime(const time_t *timer);

/* Inverse of localtime in the zone TZ selects, with the same zone errors.
 * tm_mon, tm_mday, tm_hour, tm_min and tm_sec may lie outside their ranges and
 * are normalized; tm_wday, tm_yday, tm_gmtoff and tm_zone are ignored. When
 * the wall time occurs once, a nonnegative tm_isdst that disagrees with the
 * zone reads it in the requested kind of time, using the offset of the nearest
 * period of that kind; a zone with none, such as UTC, ignores it. When the
 * wall time occurs more than once (a fold), a nonnegative tm_isdst selects the
 * only candidate whose daylight flag matches; otherwise, and for a wall time
 * skipped by a gap, fail with ENOTSUP. Success stores the normalized localtime result in *calendar and
 * preserves errno; failure returns -1, which is also a valid time, leaving
 * *calendar unchanged. EOVERFLOW when the result cannot be represented. */
time_t mktime(struct tm *calendar);

#ifdef __cplusplus
}
#endif

#endif
