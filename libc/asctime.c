#include <errno.h>
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
  if (strftime(calendar_text, sizeof(calendar_text),
      "%a %b %e %H:%M:%S %Y\n", calendar) != sizeof(calendar_text) - 1) {
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
