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
  const char *tm_zone; /* Borrowed until process exit; UTC for gmtime. */
};

#define TIME_UTC 1
#define TIME_MONOTONIC 2

/* Borrow the named startup clock with READ|PRECISE authority. time returns -1 on
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
#define CLOCK_REALTIME_COARSE 2
#define CLOCK_MONOTONIC_COARSE 3

/* POSIX clocks on the startup clock: CLOCK_REALTIME reads it as timespec_get's
 * TIME_UTC, CLOCK_MONOTONIC as TIME_MONOTONIC, requiring READ|PRECISE. The
 * explicit *_COARSE IDs need READ only and use the interrupt cache. Each quality
 * is monotonic separately; coarse may trail precise. Other IDs fail with EINVAL. */
int clock_gettime(clockid_t clock, struct timespec *result);
/* Sleep on the startup clock, requiring READ|PRECISE|SLEEP. Cooperative caught
 * events interrupt an opted-in initial thread with EINTR and actual remaining
 * duration; other failures preserve remaining. SA_RESTART does not restart
 * sleep. A tv_nsec outside 0..999999999 or negative tv_sec fails EINVAL; a
 * duration past the clock's range sleeps until its end. Workers retain ordinary
 * noninterruptible sleep. */
int nanosleep(const struct timespec *duration, struct timespec *remaining);
/* nanosleep on CLOCK_MONOTONIC or CLOCK_REALTIME, returning an error number
 * instead of setting errno. TIMER_ABSTIME sleeps until an absolute time of that
 * clock; a past time returns at once. Absolute sleeps never store remaining
 * time. The coarse clocks fail with ENOTSUP and other IDs or flags with EINVAL. */
#define TIMER_ABSTIME 1
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
    struct timespec *remaining);

/* English C-locale standard C/POSIX conversions, with E/O alternatives equal
 * to their ordinary forms. No locale selection or extended flags/widths.
 * Return the byte count excluding NUL, or zero for insufficient capacity.
 * Invalid arguments/conversions return zero with EINVAL; output on failure is
 * unspecified. %z has the standard minute precision; %Z uses tm_zone, or an
 * empty string if unknown. Neither conversion consults the current TZ. */
size_t strftime(char *__restrict output, size_t capacity,
    const char *__restrict format, const struct tm *__restrict calendar);

/* English C-locale "Www Mmm dd hh:mm:ss yyyy\n", including the trailing
 * newline and NUL in a shared static 26-byte buffer. The next successful
 * asctime or ctime overwrites it; do not free it. Not thread-safe.
 * asctime does not normalize or check consistency between date fields;
 * tm_wday, tm_mon, tm_mday, tm_hour, tm_min and tm_sec must have their standard
 * ranges (including leap-second 60). Other fields except tm_year are ignored.
 * Return NULL with EINVAL for NULL or invalid fields, EOVERFLOW for years
 * outside 0000..9999. ctime converts through localtime, with its TZ behavior
 * and errors. Success preserves errno. */
char *asctime(const struct tm *calendar);
char *ctime(const time_t *timer);

/* English C-locale parse, the inverse of strftime for the conversions below;
 * no locale selection. Returns the first unparsed byte, or NULL if the input
 * does not match, leaving *calendar unchanged. A space in format skips any
 * white space; other characters must match exactly. Supported: %a %A %b %B %h
 * (names, full or three letters, any case), %c %C %d %D %e %H %I %j %m %M %n
 * %p (applies to %I) %r %R %S %t %T %u %U %w %W %x %X %y %Y %z %% and the E and O forms, equal
 * to the ordinary ones. Numeric fields accept leading white space and range-
 * check; %y reads 69-99 as 1969-1999 and 00-68 as 2000-2068, or uses %C. %z
 * reads Z, +-hh, +-hhmm or +-hh:mm into tm_gmtoff. Only the fields read are
 * stored: tm_wday and tm_yday are not computed from a date and nothing is
 * normalized. %U and %W are validated and ignored. %Z and anything else are
 * unsupported and fail. */
char *strptime(const char *__restrict input, const char *__restrict format,
    struct tm *__restrict calendar);

/* Proleptic Gregorian UTC conversion, including negative timestamps.
 * Fail with EOVERFLOW if tm_year cannot fit in int, EINVAL for NULL inputs.
 * gmtime_r leaves the destination unchanged on failure; gmtime returns borrowed
 * per-thread storage overwritten by later successful gmtime calls on that
 * thread. No allocation.
 * Locale selection is not implemented. */
struct tm *gmtime_r(const time_t *__restrict timer, struct tm *__restrict result);
struct tm *gmtime(const time_t *timer);

/* TZ absent/empty means UTC without file access. Otherwise TZ is an IANA name
 * under boot://share/zoneinfo, using the process's boot directory capability.
 * Each call snapshots TZ from the current shared environment and retains its
 * selected zone for the whole conversion. A changed name loads that zone lazily
 * into process-owned heap storage, replacing the cache.
 * Return NULL with errno on file/allocation errors, EINVAL for invalid names or
 * malformed/unsupported TZif, ENOTSUP where zone data leaves time unspecified,
 * and EOVERFLOW when the calendar cannot be represented. No UTC fallback.
 * Success preserves errno; localtime_r leaves result unchanged on failure.
 * localtime returns per-thread storage overwritten by its next successful call
 * on that thread. tm_zone borrows an immutable designation until process exit;
 * changing TZ or replacing the cache does not invalidate it. */
struct tm *localtime_r(const time_t *__restrict timer, struct tm *__restrict result);
struct tm *localtime(const time_t *timer);

/* Inverse of localtime in one retained zone selected from the current TZ,
 * with the same zone errors.
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
