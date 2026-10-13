#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* English C-locale names only; there is no locale state to consult. */
static const char *const weekday_names[] = {
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char *const month_names[] = {
  "January", "February", "March", "April", "May", "June", "July", "August",
  "September", "October", "November", "December",
};

/* Fields are staged in the parser and applied only after the whole format
 * matched, so a failed parse leaves the caller's tm unchanged. The year forms
 * and the 12-hour clock are combined at the end. A %p without %I has no effect. */
struct parsed {
  struct tm tm;
  int year;     /* %Y */
  int two_digit; /* %y */
  int century;  /* %C */
  int hour12;   /* %I */
  int pm;       /* %p: 1 for PM, 0 for AM, -1 unseen */
  bool have_year, have_two_digit, have_century, have_hour12;
};

static const char *skip_space(const char *input)
{
  while (isspace((unsigned char)*input)) {
    ++input;
  }
  return input;
}

/* Up to digits decimal digits after optional leading space, within [low, high]. */
static const char *number(const char *input, int digits, int low, int high, int *value)
{
  input = skip_space(input);
  if (!isdigit((unsigned char)*input)) {
    return NULL;
  }
  int result = 0;
  for (int count = 0; count < digits && isdigit((unsigned char)*input); ++count) {
    result = result * 10 + (*input++ - '0');
  }
  if (result < low || result > high) {
    return NULL;
  }
  *value = result;
  return input;
}

static bool name_matches(const char *input, const char *name, size_t length)
{
  for (size_t index = 0; index < length; ++index) {
    if (tolower((unsigned char)input[index]) != tolower((unsigned char)name[index])) {
      return false;
    }
  }
  return true;
}

static size_t name_length(const char *name)
{
  size_t length = 0;
  while (name[length]) {
    ++length;
  }
  return length;
}

/* Full names or their first three letters, case-insensitive, preferring the
 * full name. Returns the index through *value. */
static const char *name_index(const char *input, const char *const *names, int count,
  int *value)
{
  input = skip_space(input);
  for (int index = 0; index < count; ++index) {
    size_t length = name_length(names[index]);
    if (name_matches(input, names[index], length)) {
      *value = index;
      return input + length;
    }
  }
  for (int index = 0; index < count; ++index) {
    if (name_matches(input, names[index], 3)) {
      *value = index;
      return input + 3;
    }
  }
  return NULL;
}

/* "Z", or +/-hh, +/-hhmm or +/-hh:mm. */
static const char *zone_offset(const char *input, long *offset)
{
  input = skip_space(input);
  if (*input == 'Z') {
    *offset = 0;
    return input + 1;
  }
  if (*input != '+' && *input != '-') {
    return NULL;
  }
  int sign = *input++ == '-' ? -1 : 1;
  int hours;
  int minutes = 0;
  if (!isdigit((unsigned char)input[0]) || !isdigit((unsigned char)input[1])) {
    return NULL;
  }
  hours = (input[0] - '0') * 10 + (input[1] - '0');
  input += 2;
  const char *minute_start = input;
  if (*minute_start == ':') {
    ++minute_start;
  }
  if (isdigit((unsigned char)minute_start[0]) && isdigit((unsigned char)minute_start[1])) {
    minutes = (minute_start[0] - '0') * 10 + (minute_start[1] - '0');
    input = minute_start + 2;
  }
  if (hours > 23 || minutes > 59) {
    return NULL;
  }
  *offset = sign * (hours * 3600L + minutes * 60L);
  return input;
}

static const char *parse(const char *input, const char *format, struct parsed *parsed);

static const char *parse_conversion(const char *input, char conversion,
  struct parsed *parsed)
{
  int value;
  switch (conversion) {
  case 'a':
  case 'A':
    input = name_index(input, weekday_names, 7, &value);
    if (input) {
      parsed->tm.tm_wday = value;
    }
    return input;
  case 'b':
  case 'B':
  case 'h':
    input = name_index(input, month_names, 12, &value);
    if (input) {
      parsed->tm.tm_mon = value;
    }
    return input;
  case 'c':
    return parse(input, "%a %b %e %H:%M:%S %Y", parsed);
  case 'C':
    input = number(input, 2, 0, 99, &value);
    if (input) {
      parsed->century = value;
      parsed->have_century = true;
    }
    return input;
  case 'd':
  case 'e':
    input = number(input, 2, 1, 31, &value);
    if (input) {
      parsed->tm.tm_mday = value;
    }
    return input;
  case 'D':
  case 'x':
    return parse(input, "%m/%d/%y", parsed);
  case 'H':
    input = number(input, 2, 0, 23, &value);
    if (input) {
      parsed->tm.tm_hour = value;
      parsed->have_hour12 = false;
    }
    return input;
  case 'I':
    input = number(input, 2, 1, 12, &value);
    if (input) {
      parsed->hour12 = value;
      parsed->have_hour12 = true;
    }
    return input;
  case 'j':
    input = number(input, 3, 1, 366, &value);
    if (input) {
      parsed->tm.tm_yday = value - 1;
    }
    return input;
  case 'm':
    input = number(input, 2, 1, 12, &value);
    if (input) {
      parsed->tm.tm_mon = value - 1;
    }
    return input;
  case 'M':
    input = number(input, 2, 0, 59, &value);
    if (input) {
      parsed->tm.tm_min = value;
    }
    return input;
  case 'n':
  case 't':
    return skip_space(input);
  case 'p':
    input = skip_space(input);
    if (name_matches(input, "AM", 2)) {
      parsed->pm = 0;
    } else if (name_matches(input, "PM", 2)) {
      parsed->pm = 1;
    } else {
      return NULL;
    }
    return input + 2;
  case 'r':
    return parse(input, "%I:%M:%S %p", parsed);
  case 'R':
    return parse(input, "%H:%M", parsed);
  case 'S':
    input = number(input, 2, 0, 60, &value);
    if (input) {
      parsed->tm.tm_sec = value;
    }
    return input;
  case 'T':
  case 'X':
    return parse(input, "%H:%M:%S", parsed);
  case 'U':
  case 'W':
    /* The week of the year is checked and then ignored, as in POSIX. */
    return number(input, 2, 0, 53, &value);
  case 'u':
    input = number(input, 1, 1, 7, &value);
    if (input) {
      parsed->tm.tm_wday = value % 7;
    }
    return input;
  case 'w':
    input = number(input, 1, 0, 6, &value);
    if (input) {
      parsed->tm.tm_wday = value;
    }
    return input;
  case 'y':
    input = number(input, 2, 0, 99, &value);
    if (input) {
      parsed->two_digit = value;
      parsed->have_two_digit = true;
    }
    return input;
  case 'Y':
    input = number(input, 4, 0, 9999, &value);
    if (input) {
      parsed->year = value;
      parsed->have_year = true;
    }
    return input;
  case 'z': {
    long offset;
    input = zone_offset(input, &offset);
    if (input) {
      parsed->tm.tm_gmtoff = offset;
    }
    return input;
  }
  case '%':
    return *input == '%' ? input + 1 : NULL;
  default:
    /* Including %Z: a zone name cannot be mapped without zone rules, and
     * ignoring it would silently read the time in the wrong zone. */
    return NULL;
  }
}

static const char *parse(const char *input, const char *format, struct parsed *parsed)
{
  while (*format && input) {
    if (isspace((unsigned char)*format)) {
      input = skip_space(input);
      ++format;
    } else if (*format != '%') {
      if (*input != *format) {
        return NULL;
      }
      ++input;
      ++format;
    } else {
      ++format;
      /* E and O select alternative locale forms, which equal the ordinary ones. */
      if (*format == 'E' || *format == 'O') {
        ++format;
      }
      if (!*format) {
        return NULL;
      }
      input = parse_conversion(input, *format++, parsed);
    }
  }
  return input;
}

char *strptime(const char *__restrict input, const char *__restrict format,
  struct tm *__restrict calendar)
{
  if (!input || !format || !calendar) {
    return NULL;
  }
  struct parsed parsed = {.tm = *calendar, .pm = -1};
  const char *end = parse(input, format, &parsed);
  if (!end) {
    return NULL;
  }
  if (parsed.have_year) {
    parsed.tm.tm_year = parsed.year - 1900;
  } else if (parsed.have_two_digit) {
    int year = parsed.have_century ? parsed.century * 100 + parsed.two_digit
      : parsed.two_digit < 69 ? 2000 + parsed.two_digit : 1900 + parsed.two_digit;
    parsed.tm.tm_year = year - 1900;
  } else if (parsed.have_century) {
    parsed.tm.tm_year = parsed.century * 100 - 1900;
  }
  if (parsed.have_hour12) {
    parsed.tm.tm_hour = parsed.hour12 % 12 + (parsed.pm == 1 ? 12 : 0);
  }
  *calendar = parsed.tm;
  return (char *)(uintptr_t)end;
}
