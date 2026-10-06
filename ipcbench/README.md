# Endpoint benchmarks

Run from the interactive shell through the session launcher:

```text
session bin://ipcbench.pxe call
session bin://ipcbench.pxe send
session bin://ipcbench.pxe call --size 0
session bin://ipcbench.pxe call --size 4096
session bin://ipcbench.pxe send --size 0
session bin://ipcbench.pxe send --size 4096
session bin://ipcbench.pxe send --size 64 --messages 8 --rounds 5
```

`session` replaces the current shell. Each example needs a fresh boot/session
after the preceding benchmark exits; it does not restart the shell.

`--size` is 0..4096 application bytes per message (default 64), `--messages` is
1..256 (default 8), and `--rounds` is 1..100 (default 5). Options accept decimal
digits and may appear once each. Baseline payload sizes are 0, 64 and 4096;
zero measures control delivery without application payload throughput. Each run
performs one untimed, verified warmup and then the requested measured passes.
Each pass, including warmup, launches a fresh receiver with fresh endpoints.
No measured request or reply carries capability attachments.

The default remains eight messages for comparison with the original baseline;
larger batches are selected explicitly with `--messages`. Endpoint delivery
records become reusable when caller collection and receipt ownership have both
ended, independently of deferred BSP storage destruction. Delivered canceled
calls retain their records until the provider finishes their receipts. Sixteen
outstanding records still exhaust capacity and return QUEUE_FULL; fresh endpoints
and the small default do not change that bound.

The same executable supplies an internal receiver process. The launcher places
it in the launching process's space on its assigned CPU. The public ABI cannot
query the current CPU number; debugger inspection can establish actual placement.
The receiver receives only memory management, endpoint creation, clock read and
a bootstrap CALL grant, with no streams, launcher or directory authority. It
creates its own receivers and returns client grants in READY, because receiver
ownership cannot transfer between processes. All reports use the sender's stderr;
stdout carries no benchmark output.

CALL uses an exported endpoint with explicit CONFIGURE, RESET, ECHO, VERIFY and
STOP operations. A measured pass sends sequential ECHO calls with same-length
replies; export dispatch is included. Round trips and confirmed request/reply
bytes are reported separately. A failed CALL reports its transport status and
delivery state, without retrying. NOT_DELIVERED does not imply rejection before
admission: an admitted queued call may expire without delivery. Bytes from a
transport-failed round trip are excluded from the confirmed byte totals.

SEND uses a raw data endpoint and a separate exported control endpoint. The
sender admits groups of at most eight data messages while the receiver waits
on the control endpoint.
It then calls DRAIN with the admitted count. The receiver drains exactly that
many data messages, retains their payload, finishes every receipt, and replies
with its cumulative consumed count. The data grant has SEND authority only.
The receiver deliberately does not drain data during an admission interval;
these results measure this bounded protocol, rather than free-running sender
and receiver concurrency.

The SEND admission result sums intervals around the group admission loops.
Completion elapsed spans all admissions, their clock boundaries and DRAIN
acknowledgments. QUEUE_FULL stops the sample: its admitted prefix is drained
through control, admitted/rejected/acknowledged-consumed counts remain visible, and rejected
data is never retried. After a failed DRAIN, acknowledged consumption is only a
confirmed lower bound: a delivered call can consume data without returning its
acknowledgment before its deadline. Any transport, count, content or cleanup failure returns
nonzero. Failed samples and zero-byte workloads have no throughput result.

Allocation, deterministic payload preparation, receiver launch/configuration,
RESET, readiness and shutdown are outside timing. Sender buffers and clock
calibration are reused across passes; receiver buffers and endpoints are fresh.
Each retained payload buffer is at most 1 MiB. Byte `offset` of message `message` is
`(message ^ (message >> 8) ^ offset ^ (offset >> 8) ^ 0xa5) & 255`.
During delivery the receiver checks metadata and copies bytes into its retained
buffer. CALL additionally retains replies in the sender. Those metadata checks,
copies, accounting and receipt operations are included in elapsed time. After
the completion timestamp, a separate VERIFY operation checks the receiver's
retained bytes/count; the sender verifies retained CALL replies independently.
Success also requires STOP acknowledgment, normal child exit and handle cleanup.

Every measured pass uses one absolute monotonic CALL deadline thirty seconds
after its initial clock reading, shared by all ECHOs or DRAIN calls. Startup
READY and each untimed control operation use separate thirty-second CALL budgets.
A timed-out delivered receipt is finished without retrying its operation. If STOP
fails, closing the last exported control client eventually supplies RETIRE to
the waiting receiver after in-flight work is reclaimed. It acknowledges the
notification and exits failed, closing its receivers and releasing queued data.
This cleanup path does not retry STOP or data delivery.
RECEIVE and process WAIT have no timeout: the CALL budgets do not guarantee a
bounded parent readiness wait or recovery from a receiver that never shuts down.

Clock-read overhead is reported as a separate batch mean and is not subtracted.
Elapsed time is wall time, not CPU time. Successful measured samples report
batch means plus median/range of completion elapsed; SEND also summarizes its
accumulated admission intervals. These are not individual operation latency
percentiles. Record accelerator, QEMU CPU/RAM configuration, host and repository
revisions when comparing results.
Clock boundaries and control delivery can dominate these short batches; their
payload rates are not a sustained throughput measurement. Fresh measured
endpoints also include their first delivery costs despite the separate warmup.
