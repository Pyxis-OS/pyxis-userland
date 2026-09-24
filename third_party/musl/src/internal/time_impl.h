#ifndef PYXIS_MUSL_TIME_IMPL_H
#define PYXIS_MUSL_TIME_IMPL_H

#include <time.h>

/* Only the pinned UTC calendar conversion is built. */
int __secs_to_tm(long long seconds, struct tm *result);

#endif
