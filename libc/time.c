#include <clock.h>
#include <errno.h>
#include <startup.h>
#include <time.h>
#include "stream.h"
#include "time_impl.h"
#include "timezone.h"

int timespec_get(struct timespec *result, int base)
{
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
  if (timezone_offset(*timer, &offset, &daylight)) {
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
  *result = converted;
  errno = saved_errno;
  return result;
}

struct tm *localtime(const time_t *timer)
{
  static struct tm result;
  return localtime_r(timer, &result);
}
