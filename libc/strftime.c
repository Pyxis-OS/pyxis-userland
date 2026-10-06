#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *const weekdays[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char *const months[] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December",
};

struct time_output {
  char *next;
  size_t remaining, length;
};

static bool append_bytes(struct time_output *output, const char *text, size_t size)
{
  if (size >= output->remaining) {
    return false;
  }
  memcpy(output->next, text, size);
  output->next += size;
  output->remaining -= size;
  output->length += size;
  return true;
}

static bool append_text(struct time_output *output, const char *text)
{
  return append_bytes(output, text, strlen(text));
}

static bool append_number(struct time_output *output, long long number,
    int width, bool space_pad)
{
  char text[32];
  int size = snprintf(text, sizeof(text), space_pad ? "%*lld" : "%0*lld",
      width, number);
  return size >= 0 && (size_t)size < sizeof(text) &&
      append_bytes(output, text, (size_t)size);
}

static bool invalid_format(void)
{
  errno = EINVAL;
  return false;
}

static bool leap_year(long long year)
{
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static int weeks_in_year(long long year, int january_weekday)
{
  return january_weekday == 4 || (january_weekday == 3 && leap_year(year)) ? 53 : 52;
}

static bool iso_week(const struct tm *calendar, long long *year, int *week)
{
  *year = (long long)calendar->tm_year + 1900;
  if (calendar->tm_wday < 0 || calendar->tm_wday > 6 ||
      calendar->tm_yday < 0 || calendar->tm_yday >= 365 + leap_year(*year)) {
    return invalid_format();
  }
  int weekday = calendar->tm_wday ? calendar->tm_wday : 7;
  int january_weekday = (calendar->tm_wday - calendar->tm_yday % 7 + 7) % 7;
  *week = (calendar->tm_yday + 11 - weekday) / 7;
  if (*week == 0) {
    --*year;
    january_weekday = (january_weekday + 6 - leap_year(*year)) % 7;
    *week = weeks_in_year(*year, january_weekday);
  } else if (*week > weeks_in_year(*year, january_weekday)) {
    ++*year;
    *week = 1;
  }
  return true;
}

static bool format_time(struct time_output *output, const char *format,
    const struct tm *calendar);

static bool format_item(struct time_output *output, char conversion,
    const struct tm *calendar)
{
  long long year = (long long)calendar->tm_year + 1900;
  int weekday = calendar->tm_wday;
  int month = calendar->tm_mon;
  switch (conversion) {
    case 'a':
    case 'A':
      if (weekday < 0 || weekday > 6) {
        return invalid_format();
      }
      return append_bytes(output, weekdays[weekday],
          conversion == 'a' ? 3 : strlen(weekdays[weekday]));
    case 'b':
    case 'h':
    case 'B':
      if (month < 0 || month > 11) {
        return invalid_format();
      }
      return append_bytes(output, months[month],
          conversion == 'B' ? strlen(months[month]) : 3);
    case 'c': return format_time(output, "%a %b %e %T %Y", calendar);
    case 'C':
      return append_number(output, year / 100, 2, false);
    case 'd': return append_number(output, calendar->tm_mday, 2, false);
    case 'D':
    case 'x': return format_time(output, "%m/%d/%y", calendar);
    case 'e': return append_number(output, calendar->tm_mday, 2, true);
    case 'F':
      if (year > 9999 && !append_text(output, "+")) {
        return false;
      }
      return format_time(output, "%Y-%m-%d", calendar);
    case 'g':
    case 'G':
    case 'V': {
      int week;
      if (!iso_week(calendar, &year, &week)) {
        return false;
      }
      if (conversion == 'V') {
        return append_number(output, week, 2, false);
      }
      if (conversion == 'g') {
        return append_number(output, (year % 100 + 100) % 100, 2, false);
      }
      return append_number(output, year, 4, false);
    }
    case 'H': return append_number(output, calendar->tm_hour, 2, false);
    case 'I':
      return append_number(output, calendar->tm_hour % 12 ? calendar->tm_hour % 12 : 12,
          2, false);
    case 'j': return append_number(output, (long long)calendar->tm_yday + 1, 3, false);
    case 'm': return append_number(output, (long long)month + 1, 2, false);
    case 'M': return append_number(output, calendar->tm_min, 2, false);
    case 'n': return append_text(output, "\n");
    case 'p': return append_text(output, calendar->tm_hour < 12 ? "AM" : "PM");
    case 'r': return format_time(output, "%I:%M:%S %p", calendar);
    case 'R': return format_time(output, "%H:%M", calendar);
    case 'S': return append_number(output, calendar->tm_sec, 2, false);
    case 't': return append_text(output, "\t");
    case 'T':
    case 'X': return format_time(output, "%H:%M:%S", calendar);
    case 'u': return append_number(output, weekday ? weekday : 7, 1, false);
    case 'U':
      return append_number(output, ((long long)calendar->tm_yday + 7 - weekday) / 7,
          2, false);
    case 'w': return append_number(output, weekday, 1, false);
    case 'W':
      return append_number(output,
          ((long long)calendar->tm_yday + 7 - (weekday + 6LL) % 7) / 7, 2, false);
    case 'y': return append_number(output, (year % 100 + 100) % 100, 2, false);
    case 'Y': return append_number(output, year, 4, false);
    case 'z': {
      if (calendar->tm_isdst < 0) {
        return true;
      }
      long offset = calendar->tm_gmtoff;
      unsigned long magnitude = offset < 0 ? 0UL - (unsigned long)offset :
          (unsigned long)offset;
      char text[32];
      int size = snprintf(text, sizeof(text), "%c%02lu%02lu", offset < 0 ? '-' : '+',
          magnitude / 3600, magnitude / 60 % 60);
      return size >= 0 && (size_t)size < sizeof(text) &&
          append_bytes(output, text, (size_t)size);
    }
    case 'Z': return append_text(output, calendar->tm_zone ? calendar->tm_zone : "");
    case '%': return append_text(output, "%");
    default: return invalid_format();
  }
}

static bool format_time(struct time_output *output, const char *format,
    const struct tm *calendar)
{
  while (*format) {
    if (*format != '%') {
      if (!append_bytes(output, format++, 1)) {
        return false;
      }
      continue;
    }
    ++format;
    if (*format == 'E' || *format == 'O') {
      char modifier = *format++;
      const char *options = modifier == 'E' ? "cCxXyY" : "deHImMSuUVwWy";
      if (!*format || !strchr(options, *format)) {
        return invalid_format();
      }
    }
    if (!*format) {
      return invalid_format();
    }
    if (!format_item(output, *format++, calendar)) {
      return false;
    }
  }
  return true;
}

size_t strftime(char *restrict output, size_t capacity,
    const char *restrict format, const struct tm *restrict calendar)
{
  if (!output || !format || !calendar) {
    errno = EINVAL;
    return 0;
  }
  if (!capacity) {
    return 0;
  }
  struct time_output buffer = {output, capacity, 0};
  if (!format_time(&buffer, format, calendar)) {
    return 0;
  }
  *buffer.next = 0;
  return buffer.length;
}
