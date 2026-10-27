#ifndef LIBC_SYS_IOCTL_H
#define LIBC_SYS_IOCTL_H

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
  unsigned short ws_row, ws_col;
  unsigned short ws_xpixel, ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Only supplied-console TIOCGWINSZ is supported. Pixel fields are unavailable
 * and zero. Neither query nor this interface confers resize authority. */
int ioctl(int descriptor, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
