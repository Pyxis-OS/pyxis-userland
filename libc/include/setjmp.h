#ifndef LIBC_SETJMP_H
#define LIBC_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Private x86-64 LP64 layout: six callee-saved integer registers, RSP and RIP.
 * The buffer is opaque to callers; its layout is defined in libc/setjmp.S. */
typedef unsigned long jmp_buf[8];

/* Returns zero initially, or the nonzero value supplied to longjmp.
 * Call directly in a controlling expression, such as if (setjmp(env) == 0),
 * or as a standalone statement, not in an initializer or assignment. */
__attribute__((returns_twice)) int setjmp(jmp_buf environment);
/* C specifies a macro; call the assembly entry directly, without a C wrapper. */
#define setjmp(environment) setjmp(environment)

/* The saving function must still be active in the same thread; do not jump
 * into a variably modified scope that has been left. Non-volatile automatic
 * locals in the saving function changed since setjmp have indeterminate values.
 * No resource cleanup or FP-environment restoration occurs. Zero becomes one. */
__attribute__((noreturn)) void longjmp(jmp_buf environment, int value);

#ifdef __cplusplus
}
#endif

#endif
