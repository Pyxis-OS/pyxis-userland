#include <errno.h>
#include <signal.h>

static int number_known(int number)
{
  return number == SIGINT || number == SIGILL || number == SIGABRT ||
    number == SIGFPE || number == SIGSEGV || number == SIGTERM;
}

void (*signal(int number, void (*handler)(int)))(int)
{
  (void)handler;
  errno = number_known(number) ? ENOTSUP : EINVAL;
  return SIG_ERR;
}

int raise(int number)
{
  errno = number_known(number) ? ENOTSUP : EINVAL;
  return -1;
}
