#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

struct integer_parse {
  unsigned long long magnitude;
  bool negative;
  bool overflow;
};

static unsigned digit_value(unsigned char character)
{
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'Z') {
    return character - 'A' + 10;
  }
  return 36;
}

static struct integer_parse parse_integer(const char *text, char **end, int base)
{
  struct integer_parse result = {0};
  if (end) {
    *end = (char *)text;
  }
  if (base && (base < 2 || base > 36)) {
    errno = EINVAL;
    return result;
  }

  const char *cursor = text;
  while (isspace((unsigned char)*cursor)) {
    ++cursor;
  }
  if (*cursor == '+' || *cursor == '-') {
    result.negative = *cursor == '-';
    ++cursor;
  }

  /* A bare prefix converts only its initial zero; leave the x/b unconsumed. */
  if (*cursor == '0') {
    if ((base == 0 || base == 16) && (cursor[1] == 'x' || cursor[1] == 'X') &&
        digit_value(cursor[2]) < 16) {
      base = 16;
      cursor += 2;
    } else if ((base == 0 || base == 2) && (cursor[1] == 'b' || cursor[1] == 'B') &&
               digit_value(cursor[2]) < 2) {
      base = 2;
      cursor += 2;
    } else if (!base) {
      base = 8;
    }
  }
  if (!base) {
    base = 10;
  }

  unsigned long long cutoff = ULLONG_MAX / (unsigned)base;
  unsigned remainder = ULLONG_MAX % (unsigned)base;
  const char *digits = cursor;
  unsigned digit;
  while ((digit = digit_value(*cursor)) < (unsigned)base) {
    if (result.magnitude > cutoff ||
        (result.magnitude == cutoff && digit > remainder)) {
      result.overflow = true;
    } else if (!result.overflow) {
      result.magnitude = result.magnitude * (unsigned)base + digit;
    }
    ++cursor;
  }

  if (end && cursor != digits) {
    *end = (char *)cursor;
  }
  return result;
}

long long strtoll(const char *restrict text, char **restrict end, int base)
{
  struct integer_parse result = parse_integer(text, end, base);
  unsigned long long limit = (unsigned long long)LLONG_MAX + result.negative;
  if (result.overflow || result.magnitude > limit) {
    errno = ERANGE;
    return result.negative ? LLONG_MIN : LLONG_MAX;
  }

  if (result.negative) {
    if (result.magnitude == (unsigned long long)LLONG_MAX + 1) {
      return LLONG_MIN;
    }
    return -(long long)result.magnitude;
  }
  return (long long)result.magnitude;
}

unsigned long long strtoull(const char *restrict text, char **restrict end, int base)
{
  struct integer_parse result = parse_integer(text, end, base);
  if (result.overflow) {
    errno = ERANGE;
    return ULLONG_MAX;
  }
  return result.negative ? 0 - result.magnitude : result.magnitude;
}

/* Pyxis LP64 gives long and long long the same signed and unsigned ranges. */
long strtol(const char *restrict text, char **restrict end, int base)
{
  return strtoll(text, end, base);
}

unsigned long strtoul(const char *restrict text, char **restrict end, int base)
{
  return strtoull(text, end, base);
}

int atoi(const char *text)
{
  return (int)strtol(text, NULL, 10);
}
