#ifndef LIBC_TIMEZONE_H
#define LIBC_TIMEZONE_H

#include <stddef.h>
#include <stdint.h>

/* POSIX rules appear inside TZif footers, never directly in the TZ setting. */
enum tz_rule_kind { TZ_DAY_OF_YEAR, TZ_JULIAN_DAY, TZ_MONTH_WEEK_DAY };

struct tz_rule {
  enum tz_rule_kind kind;
  int day, month, week, weekday;
  int seconds; /* Signed wall time relative to midnight of the selected day. */
};

/* Calendar year is years since 1900, within int range plus neighboring years. */
int __tz_year_for_secs(long long seconds, long long *year);
long long __tz_rule_to_secs(const struct tz_rule *rule, long long year);
/* Designation borrows the cache until a successful selection reload or exit;
 * default UTC borrows permanent storage. Outputs are usable only on success. */
int timezone_offset(int64_t seconds, long *offset, int *daylight,
    const char **designation);
/* Every distinct UTC offset the selected zone can report, borrowed like the
 * designation. UTC has the single offset zero. */
int timezone_offsets(const long **offsets, size_t *count);
/* The UTC offset of the period nearest SECONDS whose daylight flag is
 * DAYLIGHT: the footer rule's own offset within the rules, otherwise the
 * closest earlier or later table period. Returns 1 when the zone has no such
 * period, as UTC has no daylight time. */
int timezone_daylight_offset(int64_t seconds, int daylight, long *offset);

#endif
