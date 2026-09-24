/* Extracted from musl __tz.c; local adaptations are recorded in UPSTREAM.md. */
#include <limits.h>
#include "time_impl.h"
#include "timezone.h"

static int days_in_month(int m, int is_leap)
{
	static const unsigned char days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
	return days[m-1] + (m==2 && is_leap);
}

/* Convert a parsed POSIX DST rule and a year since 1900 to wall seconds. */
long long __tz_rule_to_secs(const struct tz_rule *rule, long long year)
{
	int is_leap;
	long long t = __year_to_secs(year, &is_leap);
	int x, m, n, d;
	if (rule->kind!=TZ_MONTH_WEEK_DAY) {
		x = rule->day;
		if (rule->kind==TZ_JULIAN_DAY && (x < 60 || !is_leap)) x--;
		t += 86400 * x;
	} else {
		m = rule->month;
		n = rule->week;
		d = rule->weekday;
		t += __month_to_secs(m-1, is_leap);
		int wday = (int)((t / 86400 + 4) % 7);
		if (wday < 0) wday += 7;
		int days = d - wday;
		if (days < 0) days += 7;
		if (n == 5 && days+28 >= days_in_month(m, is_leap)) n = 4;
		t += 86400 * (days + 7*(n-1));
	}
	t += rule->seconds;
	return t;
}

/* The Gregorian 400-year cycle averages exactly 31556952 seconds per year.
 * Keep neighboring UTC years available even at the limits of a local tm_year;
 * the final local-calendar conversion owns the exact representability check. */
int __tz_year_for_secs(long long t, long long *year)
{
	long long y = t / 31556952 + 70;
	if (y < (long long)INT_MIN-2 || y > (long long)INT_MAX+2) return -1;
	while (__year_to_secs(y, 0) > t) y--;
	while (__year_to_secs(y+1, 0) <= t) y++;
	*year = y;
	return 0;
}
