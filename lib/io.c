#include <io.h>
#include <syscall.h>

int putchar(int character)
{
  unsigned char byte = (unsigned char)character;
  if (syscall1(SYSCALL_PUTCHAR, byte) < 0) {
    return -1;
  }
  return byte;
}

int print(const char *text)
{
  for (; *text; ++text) {
    if (putchar(*text) < 0) {
      return -1;
    }
  }
  return 0;
}
