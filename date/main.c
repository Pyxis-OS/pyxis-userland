#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
  (void)argv;
  if (argc != 1) {
    fputs("usage: date\n", stderr);
    return EXIT_FAILURE;
  }

  struct timespec now;
  struct tm calendar;
  if (!timespec_get(&now, TIME_UTC) || !gmtime_r(&now.tv_sec, &calendar)) {
    perror("date");
    return EXIT_FAILURE;
  }
  int64_t year = (int64_t)calendar.tm_year + 1900;
  if (year < 0 || year > 9999) {
    fputs("date: year is outside the four-digit display range\n", stderr);
    return EXIT_FAILURE;
  }
  if (printf("%04d-%02d-%02dT%02d:%02d:%02dZ\n", (int)year,
      calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour,
      calendar.tm_min, calendar.tm_sec) < 0) {
    perror("date: output");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
