#ifndef USERSPACE_IO_H
#define USERSPACE_IO_H

/* Unbuffered output through syscall 0; no libc or stream state.
 * putchar returns the unsigned byte written, or -1 on failure.
 * print takes a valid NUL-terminated string, adds no newline, and returns
 * zero on success or -1 after the first failed character. */
int putchar(int character);
int print(const char *text);

#endif
