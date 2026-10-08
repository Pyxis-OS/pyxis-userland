#include <locale.h>
#include <stddef.h>
#include <string.h>

static char c_locale_name[] = "C";

char *setlocale(int category, const char *locale)
{
  if (category < LC_ALL || category > LC_TIME) {
    return NULL;
  }
  if (locale != NULL && locale[0] != '\0' && strcmp(locale, "C") != 0) {
    return NULL;
  }
  return c_locale_name;
}
