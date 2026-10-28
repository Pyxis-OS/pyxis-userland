#ifndef LIBC_TERMIOS_H
#define LIBC_TERMIOS_H

#include <stdint.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

#define NCCS 32
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXOFF 0010000
#define IXANY 0004000
#define OPOST 0000001
#define ONLCR 0000004
#define CSIZE 0000060
#define CS5 0000000
#define CS6 0000020
#define CS7 0000040
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define PARODD 0001000
#define HUPCL 0002000
#define CLOCAL 0004000
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* Byte-only, unechoed profile. Only ISIG and MIN/TIME may change. ISIG
 * withdraws this owner's PASSTHROUGH; it neither arms input nor delivers a
 * signal. Other owners can keep PASSTHROUGH active. Attributes belong to one
 * libc open object, shared by dup aliases, not a global terminal mode.
 * O_NONBLOCK supersedes MIN/TIME for transfers without changing the stored
 * timings; mode snapshots/restoration never capture or change NONBLOCK.
 *
 * Values are copyable snapshots with no allocation to release. Preserve the
 * private identity when modifying/restoring a tcgetattr result: a replaced or
 * closed open object is rejected with EBADF, including after address reuse. */
struct termios {
  tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
  cc_t c_cc[NCCS];
  uint64_t __pyxis_identity;
};

#ifdef __cplusplus
extern "C" {
#endif

int tcgetattr(int descriptor, struct termios *attributes);
/* DRAIN/FLUSH fence native output for at most 30 seconds. FLUSH then discards
 * queued input and libc read-ahead; modes stay unchanged if either fails.
 * An active reader can cause EBUSY. No output is discarded on timeout.
 * An admitted drain holds mode exclusion for its bounded native wait; final
 * close can wait for that operation, then releases the owner's authority.
 * Flush changes input already pending; it does not clear FILE pushback/EOF. */
int tcsetattr(int descriptor, int action, const struct termios *attributes);
void cfmakeraw(struct termios *attributes);
int tcdrain(int descriptor);

#ifdef __cplusplus
}
#endif

#endif
