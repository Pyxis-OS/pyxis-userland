#ifndef PYXIS_MUSL_TIME_IMPL_H
#define PYXIS_MUSL_TIME_IMPL_H

#include <time.h>

/* Pinned calendar arithmetic, shared by UTC and local-time conversion. */
long long __year_to_secs(long long year, int *is_leap);
int __month_to_secs(int month, int is_leap);
int __secs_to_tm(long long seconds, struct tm *result);
/* Normalizes tm_mon into the year; other fields are added without limits. */
long long __tm_to_secs(const struct tm *calendar);

#endif
