#include <errno.h>
#include <stdio.h>
#include <time.h>

static char calendar_text[26];

char *asctime(const struct tm *calendar)
{
  if (!calendar || calendar->tm_wday < 0 || calendar->tm_wday > 6 ||
      calendar->tm_mon < 0 || calendar->tm_mon > 11 ||
      calendar->tm_mday < 1 || calendar->tm_mday > 31 ||
      calendar->tm_hour < 0 || calendar->tm_hour > 23 ||
      calendar->tm_min < 0 || calendar->tm_min > 59 ||
      calendar->tm_sec < 0 || calendar->tm_sec > 60) {
    errno = EINVAL;
    return NULL;
  }
  if (calendar->tm_year < -1900 || calendar->tm_year > 8099) {
    errno = EOVERFLOW;
    return NULL;
  }
  static const char days[][4] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  static const char months[][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  int length = snprintf(calendar_text, sizeof(calendar_text),
      "%s %s%3d %.2d:%.2d:%.2d %d\n", days[calendar->tm_wday], months[calendar->tm_mon],
      calendar->tm_mday, calendar->tm_hour, calendar->tm_min, calendar->tm_sec,
      calendar->tm_year + 1900);
  if (length < 0 || (size_t)length >= sizeof(calendar_text)) {
    errno = EOVERFLOW;
    return NULL;
  }
  return calendar_text;
}

char *ctime(const time_t *timer)
{
  struct tm *calendar = localtime(timer);
  return calendar ? asctime(calendar) : NULL;
}
