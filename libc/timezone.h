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
struct timezone_data;
struct timezone_snapshot {
  struct timezone_data *zone;
};

/* Retain one selected zone across conversion, allocation and concurrent TZ
 * changes. Close once. Failed selection never falls back to the cached zone. */
int timezone_snapshot_init(struct timezone_snapshot *snapshot);
void timezone_snapshot_close(struct timezone_snapshot *snapshot);
/* Intern a designation until process exit, for an escaping tm_zone pointer. */
const char *timezone_designation(const char *name);
/* Offsets and raw designations borrow the retained snapshot. */
int timezone_offset(const struct timezone_snapshot *snapshot, int64_t seconds,
    long *offset, int *daylight, const char **designation);
int timezone_offsets(const struct timezone_snapshot *snapshot,
    const long **offsets, size_t *count);
/* Offset in the nearest period with the requested daylight flag; 1 means no
 * such period exists, for example daylight time in UTC. */
int timezone_daylight_offset(const struct timezone_snapshot *snapshot,
    int64_t seconds, int daylight, long *offset);

#endif
