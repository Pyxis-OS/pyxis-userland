#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
  bool utc = argc == 2 && !strcmp(argv[1], "-u");
  if (argc != 1 && !utc) {
    fputs("usage: date [-u]\n", stderr);
    return EXIT_FAILURE;
  }

  struct timespec now;
  struct tm calendar;
  if (!timespec_get(&now, TIME_UTC) ||
      !(utc ? gmtime_r(&now.tv_sec, &calendar) :
              localtime_r(&now.tv_sec, &calendar))) {
    perror("date");
    return EXIT_FAILURE;
  }
  int64_t year = (int64_t)calendar.tm_year + 1900;
  if (year < 0 || year > 9999) {
    fputs("date: year is outside the four-digit display range\n", stderr);
    return EXIT_FAILURE;
  }
  char suffix[32] = "Z";
  if (!utc) {
    long offset = calendar.tm_gmtoff;
    char sign = offset < 0 ? '-' : '+';
    if (offset < 0) {
      offset = -offset;
    }
    /* Historical zones can have second-resolution offsets; never round them. */
    if (offset % 60) {
      snprintf(suffix, sizeof(suffix), "%c%02ld:%02ld:%02ld", sign,
          offset / 3600, offset / 60 % 60, offset % 60);
    } else {
      snprintf(suffix, sizeof(suffix), "%c%02ld:%02ld", sign,
          offset / 3600, offset / 60 % 60);
    }
  }
  if (printf("%04d-%02d-%02dT%02d:%02d:%02d%s\n", (int)year,
      calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour,
      calendar.tm_min, calendar.tm_sec, suffix) < 0) {
    perror("date: output");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
