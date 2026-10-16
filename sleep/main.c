#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NANOSECOND_DIGITS 9
#define NANOSECONDS_PER_SECOND 1000000000L

static bool is_digit(char c)
{
  return c >= '0' && c <= '9';
}

/*
 * Accepts DIGITS, DIGITS.DIGITS or .DIGITS. Fraction digits beyond the first
 * nine round the duration up, so the sleep never ends early.
 */
static bool parse_duration(const char *text, struct timespec *duration)
{
  int64_t seconds = 0;
  int64_t nanoseconds = 0;
  size_t integer_digits = 0;
  for (; is_digit(*text); ++text, ++integer_digits) {
    int digit = *text - '0';
    if (seconds > (INT64_MAX - digit) / 10) {
      return false;
    }
    seconds = seconds * 10 + digit;
  }

  size_t fraction_digits = 0;
  bool round_up = false;
  if (*text == '.') {
    ++text;
    int64_t scale = NANOSECONDS_PER_SECOND / 10;
    for (; is_digit(*text); ++text, ++fraction_digits) {
      int digit = *text - '0';
      if (fraction_digits < NANOSECOND_DIGITS) {
        nanoseconds += digit * scale;
        scale /= 10;
      } else if (digit) {
        round_up = true;
      }
    }
    if (!fraction_digits) {
      return false;
    }
  }
  if (*text || (!integer_digits && !fraction_digits)) {
    return false;
  }

  if (round_up && ++nanoseconds == NANOSECONDS_PER_SECOND) {
    if (seconds == INT64_MAX) {
      return false;
    }
    ++seconds;
    nanoseconds = 0;
  }
  duration->tv_sec = (time_t)seconds;
  duration->tv_nsec = (long)nanoseconds;
  return true;
}

int main(int argc, char **argv)
{
  struct timespec duration;
  if (argc != 2 || !parse_duration(argv[1], &duration)) {
    fputs("usage: sleep SECONDS\n", stderr);
    return EXIT_FAILURE;
  }
  if (nanosleep(&duration, NULL)) {
    perror("sleep");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
