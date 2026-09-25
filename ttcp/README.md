# Native ttcp transmitter

`ttcp -t [-p PORT] [-n BUFFERS] [-l BYTES] HOST` sends a finite printable-ASCII
pattern through Pyxis stream capabilities. Defaults are port 5001, 2048 buffers
and 8192 bytes per buffer (16 MiB). Options accept attached or separate decimal
values, all positive; counts/lengths fit 32 bits and the total uses 64 bits.
It allocates one source buffer. Hostnames use the shared DNS helper.

Timing excludes DNS, allocation and connection setup. It covers all writes,
write shutdown, peer EOF and orderly transport closure, including any final
FIN-ACK wait. Connect and each write have fresh ten-second deadlines; one
additional ten-second deadline covers the whole completion phase. After EOF,
INSPECT checks CLOSED, both shutdown flags and no terminal error, sleeping up
to 10 ms between checks. TIME_WAIT counts as closed without waiting for its
expiry. A failed run reports only locally accepted bytes and exits unsuccessfully.
The host receiver's byte count establishes application receipt. Guest MiB/s
includes closure time and is not a pure link-throughput measurement.

This is a first-party transmitter, not the historical socket-based program.
There is no guest receive mode, UDP mode, stdin source, socket tuning or CPU-use
accounting. An ordinary classic `ttcp -r` receiver sinks the bytes and closes
on EOF; its count can be compared with the guest's configured total.

## Reference and provenance

Command conventions and the repeating printable ASCII pattern follow the classic
public-domain ttcp, inspected at troglobit/mtools commit
`a1f010f5118f5e9c3d19d06c78b48ca06a984ed0`, file
[`ttcp.c`](https://github.com/troglobit/mtools/blob/a1f010f5118f5e9c3d19d06c78b48ca06a984ed0/ttcp.c).
Its notice states “Public Domain. Distribution Unlimited.” Credits name
T. C. Slattery, Mike Muuss, Silicon Graphics, Yvan Pointurier and Joachim Nilsson.
No upstream source file is vendored or compiled into the guest; parsing, native
calls, ownership, timing and completion handling are implemented here.
