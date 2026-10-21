#ifndef USERSPACE_COMMON_SYSTEM_LOG_H
#define USERSPACE_COMMON_SYSTEM_LOG_H

/* Writes one formatted line to the kernel log at info level, never to the
 * caller's terminal. The only user write path is the LOG_PUTCHAR system call,
 * which has no level, so a caller cannot mark a warning. Text longer than the
 * internal buffer is truncated. The caller supplies the newline. */
void system_log(const char *format, ...);

#endif
