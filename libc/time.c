#include <clock.h>
#include <errno.h>
#include <startup.h>
#include <time.h>
#include "errors.h"
#include "time_impl.h"
#include "timezone.h"

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

static int monotonic_get(struct timespec *result)
{
  uint64_t nanoseconds;
  enum call_status status = clock_now(startup_resource("clock"), &nanoseconds);
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
  enum call_status status = clock_wall_now(startup_resource("clock"), &reading);
  if (status != CALL_OK) {
    errno = libc_call_errno(status);
    return 0;
  }
  *result = (struct timespec){reading.seconds, (long)reading.nanoseconds};
  return TIME_UTC;
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
  static struct tm result;
  return gmtime_r(timer, &result);
}

struct tm *localtime_r(const time_t *restrict timer, struct tm *restrict result)
{
  if (!timer || !result) {
    errno = EINVAL;
    return NULL;
  }
  int saved_errno = errno;
  long offset;
  int daylight;
  const char *designation;
  if (timezone_offset(*timer, &offset, &daylight, &designation)) {
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
  converted.tm_zone = designation;
  *result = converted;
  errno = saved_errno;
  return result;
}

struct tm *localtime(const time_t *timer)
{
  static struct tm result;
  return localtime_r(timer, &result);
}
