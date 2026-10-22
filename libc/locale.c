#include <locale.h>
#include <langinfo.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

static char c_locale_name[] = "C";

struct lconv *localeconv(void)
{
  static char decimal[] = ".", empty[] = "";
  static struct lconv data = {
    .decimal_point = decimal, .thousands_sep = empty, .grouping = empty,
    .int_curr_symbol = empty, .currency_symbol = empty, .mon_decimal_point = empty,
    .mon_thousands_sep = empty, .mon_grouping = empty,
    .positive_sign = empty, .negative_sign = empty,
    .int_frac_digits = CHAR_MAX, .frac_digits = CHAR_MAX,
    .p_cs_precedes = CHAR_MAX, .p_sep_by_space = CHAR_MAX,
    .n_cs_precedes = CHAR_MAX, .n_sep_by_space = CHAR_MAX,
    .p_sign_posn = CHAR_MAX, .n_sign_posn = CHAR_MAX,
    .int_p_cs_precedes = CHAR_MAX, .int_p_sep_by_space = CHAR_MAX,
    .int_n_cs_precedes = CHAR_MAX, .int_n_sep_by_space = CHAR_MAX,
    .int_p_sign_posn = CHAR_MAX, .int_n_sign_posn = CHAR_MAX,
  };
  return &data;
}

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

char *nl_langinfo(nl_item item)
{
  static char codeset[] = "UTF-8", empty[] = "";
  return item == CODESET ? codeset : empty;
}
