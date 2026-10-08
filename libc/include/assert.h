#ifndef LIBC_ASSERT_H
#define LIBC_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((noreturn)) void __assert_fail(const char *expression,
  const char *file, int line, const char *function);

#ifdef __cplusplus
}
#endif

#endif

/* Re-including this header must honor the current value of NDEBUG. */
#undef assert
#ifdef NDEBUG
#define assert(...) ((void)0)
#else
#define assert(...) ((__VA_ARGS__) ? (void)0 : \
  __assert_fail(#__VA_ARGS__, __FILE__, __LINE__, __func__))
#endif
