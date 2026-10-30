#include <clock.h>
#include <errno.h>
#include <startup.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "errors.h"
#include "time_impl.h"
#include "timezone.h"
#include "thread_private.h"
#include "thread_platform.h"
#include "signal_internal.h"
#include "signal_scope.h"
#include <wait.h>

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)
#define MICROSECONDS_PER_SECOND UINT64_C(1000000)
#define NANOSECONDS_PER_MICROSECOND UINT64_C(1000)

static int monotonic_get(struct timespec *result)
{
  uint64_t nanoseconds;
  enum call_status status = clock_now(libc_thread_clock(), &nanoseconds);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return 0;
  }
  *result = (struct timespec){
    (time_t)(nanoseconds / NANOSECONDS_PER_SECOND),
    (long)(nanoseconds % NANOSECONDS_PER_SECOND),
  };
  return TIME_MONOTONIC;
}

int timespec_get(struct timespec *result, int base)
{
  if (result && base == TIME_MONOTONIC) {
    return monotonic_get(result);
  }
  if (!result || base != TIME_UTC) {
    errno = EINVAL;
    return 0;
  }
  struct clock_wall_reading reading;
  enum call_status status = clock_wall_now(libc_thread_clock(), &reading);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return 0;
  }
  *result = (struct timespec){reading.seconds, (long)reading.nanoseconds};
  return TIME_UTC;
}

int clock_gettime(clockid_t clock, struct timespec *result)
{
  if (clock == CLOCK_REALTIME_COARSE || clock == CLOCK_MONOTONIC_COARSE) {
    if (!result) {
      errno = EINVAL;
      return -1;
    }
    struct timespec reading;
    enum call_status status;
    if (clock == CLOCK_REALTIME_COARSE) {
      struct clock_wall_reading wall;
      status = clock_wall_coarse_now(libc_thread_clock(), &wall);
      reading = (struct timespec){wall.seconds, (long)wall.nanoseconds};
    } else {
      uint64_t nanoseconds;
      status = clock_coarse_now(libc_thread_clock(), &nanoseconds);
      reading = (struct timespec){(time_t)(nanoseconds / NANOSECONDS_PER_SECOND),
          (long)(nanoseconds % NANOSECONDS_PER_SECOND)};
    }
    if (status != CALL_OK) {
      errno = libc_call_errno(status);
      return -1;
    }
    *result = reading;
    return 0;
  }
  int base = clock == CLOCK_REALTIME ? TIME_UTC :
             clock == CLOCK_MONOTONIC ? TIME_MONOTONIC : 0;
  if (!base || !result) {
    errno = EINVAL;
    return -1;
  }
  return timespec_get(result, base) ? 0 : -1;
}

int gettimeofday(struct timeval *restrict result, void *restrict timezone)
{
  if (!result) {
    errno = EINVAL;
    return -1;
  }
  if (timezone) {
    errno = ENOTSUP;
    return -1;
  }
  struct timespec reading;
  if (clock_gettime(CLOCK_REALTIME, &reading) < 0) {
    return -1;
  }
  *result = (struct timeval){reading.tv_sec,
      reading.tv_nsec / NANOSECONDS_PER_MICROSECOND};
  return 0;
}

/* Saturate: a deadline past the clock's range sleeps until its end. */
static uint64_t deadline_after(uint64_t now, const struct timespec *duration)
{
  uint64_t seconds = (uint64_t)duration->tv_sec;
  if (seconds <= (UINT64_MAX - now) / NANOSECONDS_PER_SECOND) {
    uint64_t span = seconds * NANOSECONDS_PER_SECOND;
    if ((uint64_t)duration->tv_nsec <= UINT64_MAX - now - span) {
      return now + span + (uint64_t)duration->tv_nsec;
    }
  }
  return UINT64_MAX;
}

/* Sleep on the startup clock until a monotonic deadline; returns an error
 * number. An interruption stores the actual remaining time when requested. */
static int sleep_until(handle_t clock, uint64_t now, uint64_t deadline,
    struct timespec *remaining)
{
  enum call_status status;
  struct libc_signal_wait signals;
  if (libc_signal_prepare(&signals) < 0) {
    return errno;
  }
  int error = 0;
  if ((!signals.count && !signals.pending) || now >= deadline) {
    status = clock_sleep_until(libc_thread_sleep_clock(), deadline);
    error = status == CALL_OK ? 0 : libc_call_errno(status);
  } else {
    /* Preserve sleep authority before using readiness for an interruptible
     * deadline. A past native sleep validates SLEEP without parking. */
    status = clock_sleep_until(libc_thread_sleep_clock(), now);
    error = status == CALL_OK ? 0 : libc_call_errno(status);
    while (!error) {
      if (signals.pending) {
        error = EINTR;
        break;
      }
      if (!signals.count) {
        status = clock_sleep_until(libc_thread_sleep_clock(), deadline);
        error = status == CALL_OK ? 0 : libc_call_errno(status);
        break;
      }
      status = clock_now(clock, &now);
      if (status != CALL_OK) {
        error = libc_call_errno(status);
        break;
      }
      if (now >= deadline) {
        break;
      }
      uint64_t until = now > UINT64_MAX - WAIT_MAX_WAIT_NS ? UINT64_MAX : now + WAIT_MAX_WAIT_NS;
      if (until > deadline) {
        until = deadline;
      }
      uint64_t events[2] = {0};
      status = wait_many(signals.interests, signals.count, until, events);
      if (status != CALL_OK && status != CALL_TIMED_OUT) {
        error = libc_call_errno(status);
        break;
      }
      if (libc_signal_prepare(&signals) < 0) {
        error = errno;
        break;
      }
    }
  }
  if (error == EINTR && remaining) {
    status = clock_now(clock, &now);
    if (status != CALL_OK) {
      error = libc_call_errno(status);
    } else {
      uint64_t left = deadline > now ? deadline - now : 0;
      *remaining = (struct timespec){left / NANOSECONDS_PER_SECOND, left % NANOSECONDS_PER_SECOND};
    }
  }
  libc_signal_checkpoint();
  return error;
}

static bool valid_duration(const struct timespec *duration)
{
  return duration && duration->tv_sec >= 0 && duration->tv_nsec >= 0 &&
      duration->tv_nsec < (long)NANOSECONDS_PER_SECOND;
}

int nanosleep(const struct timespec *duration, struct timespec *remaining)
{
  if (!valid_duration(duration)) {
    errno = EINVAL;
    return -1;
  }
  handle_t clock = libc_thread_clock();
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return -1;
  }
  int error = sleep_until(clock, now, deadline_after(now, duration), remaining);
  if (error) {
    errno = error;
  }
  return error ? -1 : 0;
}

int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec *request,
    struct timespec *remaining)
{
  if (clock_id == CLOCK_REALTIME_COARSE || clock_id == CLOCK_MONOTONIC_COARSE) {
    return ENOTSUP;
  }
  if ((clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) ||
      (flags & ~TIMER_ABSTIME) || !valid_duration(request)) {
    return EINVAL;
  }
  handle_t clock = libc_thread_clock();
  uint64_t now;
  enum call_status status = clock_now(clock, &now);
  if (status != CALL_OK) {
    return libc_call_errno(status);
  }
  if (!(flags & TIMER_ABSTIME)) {
    return sleep_until(clock, now, deadline_after(now, request), remaining);
  }
  if (clock_id == CLOCK_MONOTONIC) {
    uint64_t seconds = (uint64_t)request->tv_sec;
    uint64_t deadline = seconds > UINT64_MAX / NANOSECONDS_PER_SECOND ? UINT64_MAX :
        seconds * NANOSECONDS_PER_SECOND;
    deadline = deadline > UINT64_MAX - (uint64_t)request->tv_nsec ? UINT64_MAX :
        deadline + (uint64_t)request->tv_nsec;
    return sleep_until(clock, now, deadline, NULL);
  }
  /* The wall clock is never resynchronized, so a realtime deadline is a fixed
   * offset from the monotonic clock. */
  struct clock_wall_reading wall;
  status = clock_wall_now(clock, &wall);
  if (status != CALL_OK) {
    return libc_call_errno(status);
  }
  struct timespec left = {request->tv_sec - wall.seconds,
      request->tv_nsec - (long)wall.nanoseconds};
  if (left.tv_nsec < 0) {
    left.tv_nsec += (long)NANOSECONDS_PER_SECOND;
    --left.tv_sec;
  }
  if (left.tv_sec < 0) {
    left = (struct timespec){0, 0};
  }
  return sleep_until(clock, now, deadline_after(now, &left), NULL);
}

int usleep(useconds_t microseconds)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  struct timespec duration = {
    .tv_sec = microseconds / MICROSECONDS_PER_SECOND,
    .tv_nsec = (microseconds % MICROSECONDS_PER_SECOND) * NANOSECONDS_PER_MICROSECOND,
  };
  return nanosleep(&duration, NULL);
}

unsigned int sleep(unsigned int seconds)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  if (!seconds) {
    return 0;
  }
  struct timespec duration = {.tv_sec = seconds}, remaining = duration;
  if (nanosleep(&duration, &remaining) == 0) {
    return 0;
  }
  return (unsigned int)remaining.tv_sec + (remaining.tv_nsec != 0);
}

time_t time(time_t *result)
{
  struct timespec reading;
  time_t seconds = timespec_get(&reading, TIME_UTC) ? reading.tv_sec : (time_t)-1;
  if (result) {
    *result = seconds;
  }
  return seconds;
}

double difftime(time_t end, time_t beginning)
{
  bool negative = end < beginning;
  /* Unsigned subtraction represents even the full INT64_MIN..INT64_MAX span,
   * and preserves small differences before conversion to double. */
  uint64_t difference = negative ? (uint64_t)beginning - (uint64_t)end :
      (uint64_t)end - (uint64_t)beginning;
  return negative ? -(double)difference : (double)difference;
}

struct tm *gmtime_r(const time_t *restrict timer, struct tm *restrict result)
{
  if (!timer || !result) {
    errno = EINVAL;
    return NULL;
  }
  struct tm converted = {0};
  if (__secs_to_tm(*timer, &converted) < 0) {
    errno = EOVERFLOW;
    return NULL;
  }
  converted.tm_zone = "UTC";
  *result = converted;
  return result;
}

struct tm *gmtime(const time_t *timer)
{
  return gmtime_r(timer, &libc_thread_record()->utc_result);
}

static struct tm *localtime_snapshot(const struct timezone_snapshot *zone,
    const time_t *timer, struct tm *result)
{
  if (!timer || !result) {
    errno = EINVAL;
    return NULL;
  }
  int saved_errno = errno;
  long offset;
  int daylight;
  const char *designation;
  if (timezone_offset(zone, *timer, &offset, &daylight, &designation)) {
    return NULL;
  }
  int64_t local;
  struct tm converted = {0};
  if (__builtin_add_overflow(*timer, offset, &local) ||
      __secs_to_tm(local, &converted) < 0) {
    errno = EOVERFLOW;
    return NULL;
  }
  converted.tm_isdst = daylight;
  converted.tm_gmtoff = offset;
  converted.tm_zone = timezone_designation(designation);
  if (!converted.tm_zone) {
    return NULL;
  }
  *result = converted;
  errno = saved_errno;
  return result;
}

struct tm *localtime_r(const time_t *restrict timer, struct tm *restrict result)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  if (!timer || !result) {
    errno = EINVAL;
    return NULL;
  }
  int saved_errno = errno;
  struct timezone_snapshot zone;
  if (timezone_snapshot_init(&zone)) {
    return NULL;
  }
  struct tm *converted = localtime_snapshot(&zone, timer, result);
  int error = errno;
  timezone_snapshot_close(&zone);
  errno = converted ? saved_errno : error;
  return converted;
}

struct tm *localtime(const time_t *timer)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  return localtime_r(timer, &libc_thread_record()->local_result);
}

static time_t make_time(const struct timezone_snapshot *zone, struct tm *calendar)
{
  if (!calendar) {
    errno = EINVAL;
    return -1;
  }
  int saved_errno = errno;
  const long *offsets;
  size_t offset_count;
  if (timezone_offsets(zone, &offsets, &offset_count)) {
    return -1;
  }

  /* A UTC instant T shows this wall time if the zone's offset at T is exactly
   * WALL - T. Every offset the zone can report gives one candidate: none
   * matches inside a gap, several match in a fold. */
  int64_t wall = __tm_to_secs(calendar);
  size_t matches = 0, selected = 0;
  int64_t match_time = 0, selected_time = 0;
  int match_daylight = 0;
  for (size_t i = 0; i < offset_count; ++i) {
    int64_t candidate;
    long offset;
    int daylight;
    const char *designation;
    if (__builtin_sub_overflow(wall, offsets[i], &candidate)) {
      errno = EOVERFLOW;
      return -1;
    }
    if (timezone_offset(zone, candidate, &offset, &daylight, &designation)) {
      /* An instant the zone data leaves unspecified shows no wall time. */
      if (errno == ENOTSUP) {
        continue;
      }
      return -1;
    }
    if (offset != offsets[i]) {
      continue;
    }
    ++matches;
    match_time = candidate;
    match_daylight = daylight;
    if (calendar->tm_isdst >= 0 && !daylight == !calendar->tm_isdst) {
      ++selected;
      selected_time = candidate;
    }
  }
  /* Gaps and folds that tm_isdst cannot settle are a profile restriction. */
  if (!matches || (matches > 1 && selected != 1)) {
    errno = ENOTSUP;
    return -1;
  }
  time_t result = matches == 1 ? match_time : selected_time;

  /* A tm_isdst that disagrees with the only match asks for the wall time in
   * the other kind of time, as C specifies: use the offset of the nearest
   * period of that kind. A zone without one, such as UTC, keeps the match. */
  if (matches == 1 && calendar->tm_isdst >= 0 &&
      !match_daylight != !calendar->tm_isdst) {
    long requested;
    int found = timezone_daylight_offset(zone, match_time, calendar->tm_isdst > 0, &requested);
    if (found < 0) {
      return -1;
    }
    if (!found && __builtin_sub_overflow(wall, requested, &result)) {
      errno = EOVERFLOW;
      return -1;
    }
  }

  struct tm converted;
  if (!localtime_snapshot(zone, &result, &converted)) {
    return -1;
  }
  *calendar = converted;
  errno = saved_errno;
  return result;
}

time_t mktime(struct tm *calendar)
{
  struct libc_signal_scope signals __attribute__((cleanup(libc_signal_scope_end))) =
      libc_signal_scope_begin();
  if (!calendar) {
    errno = EINVAL;
    return -1;
  }
  int saved_errno = errno;
  struct timezone_snapshot zone;
  if (timezone_snapshot_init(&zone)) {
    return -1;
  }
  errno = saved_errno;
  time_t result = make_time(&zone, calendar);
  int error = errno;
  timezone_snapshot_close(&zone);
  errno = error;
  return result;
}
