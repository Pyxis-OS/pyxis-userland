#ifndef LIBC_LOCALE_H
#define LIBC_LOCALE_H

#ifdef __cplusplus
extern "C" {
#endif

#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5

struct lconv {
  char *decimal_point, *thousands_sep, *grouping;
  char *int_curr_symbol, *currency_symbol, *mon_decimal_point;
  char *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space;
  char n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn;
  char int_p_cs_precedes, int_p_sep_by_space, int_n_cs_precedes;
  char int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};

/* Only the "C" locale exists. A NULL locale queries and returns "C"; "" (the
 * native environment) and "C" select it and return "C". Any other name, or an
 * unknown category, returns NULL and leaves the locale unchanged. The returned
 * string must not be modified. */
char *setlocale(int category, const char *locale);
/* Static C-locale data; neither the record nor its strings may be modified.
 * No grouping/currency information; unspecified monetary fields are CHAR_MAX. */
struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif
