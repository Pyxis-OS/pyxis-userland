#include <errno.h>
#include <limits.h>
#include <pyxis/terminal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <termios.h>

void cfmakeraw(struct termios *attributes)
{
  attributes->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  attributes->c_oflag &= ~OPOST;
  attributes->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  attributes->c_cflag = (attributes->c_cflag & ~(CSIZE | PARENB)) | CS8;
  attributes->c_cc[VMIN] = 1;
  attributes->c_cc[VTIME] = 0;
}

int ioctl(int descriptor, unsigned long request, ...)
{
  struct console_size_reply size;
  if (pyxis_terminal_get_size(descriptor, &size) < 0) {
    return -1;
  }
  if (request != TIOCGWINSZ) {
    errno = ENOTSUP;
    return -1;
  }
  va_list arguments;
  va_start(arguments, request);
  struct winsize *result = va_arg(arguments, struct winsize *);
  va_end(arguments);
  if (!result) {
    errno = EINVAL;
    return -1;
  }
  if (size.columns > USHRT_MAX || size.rows > USHRT_MAX) {
    errno = EOVERFLOW;
    return -1;
  }
  *result = (struct winsize){.ws_col = size.columns, .ws_row = size.rows};
  return 0;
}
