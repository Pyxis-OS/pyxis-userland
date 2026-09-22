#ifndef LIBC_TLSF_USER_H
#define LIBC_TLSF_USER_H

#include <limits.h>
#include <stddef.h>
#include <string.h>

void libc_tlsf_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void libc_tlsf_fail(const char *condition, const char *file, int line);

#define tlsf_assert(condition) \
  ((condition) ? (void)0 : libc_tlsf_fail(#condition, __FILE__, __LINE__))
#define printf libc_tlsf_log

#endif
