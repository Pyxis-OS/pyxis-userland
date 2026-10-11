# Native thread qualification

Build with a selected SDK exporting the native thread and guarded-memory ABI:

```sh
make SDK=/path/to/sdk threadprobe
```

Run manually from an interactive shell. `session` delegates the `threads`
resource; ordinary foreground and background launches do not. Each invocation
replaces the shell and needs a fresh session afterward.

```text
session bin://threadprobe.pxe lifecycle
session bin://threadprobe.pxe inspect
session bin://threadprobe.pxe bind
session bin://threadprobe.pxe refusal
session bin://threadprobe.pxe exhaust
session bin://threadprobe.pxe stop
session bin://threadprobe.pxe fault
session bin://threadprobe.pxe close
```

`lifecycle` creates two siblings with independent eager 64 KiB native stacks and
one-page raw FS contexts, shares an atomic counter, checks captured RSP/FS and
inherited MXCSR, waits twice, polls completion readiness, then releases backing.
Closing a copied observer while its sibling is live leaves execution intact.
These sizes are probe inputs, independent of future libc convenience policy.

`inspect` holds both siblings until Enter. It prints prepared stack/FS addresses
before creating them. Use GDB to inspect the process's task set, their shared CR3,
distinct RSP/FS and guarded mappings. `contexts` and `shared_count` are ordinary
shared globals in the debug ELF. User observations alone do not establish CR3 or
CPU placement. Repeat the ordinary modes on one and four CPUs.

`bind` binds the initial thread's FS once, checks rebind and pinned release return
BUSY, and leaves that context for process retirement. `refusal` checks pinned
release, duplicate live stack/FS identities, stale identity and NX entry refusal;
successful subsequent creation checks provisional pin unwind. No language TLS,
TCB, errno or libc startup change is supplied by these raw FS contexts.

`exhaust` prepares its contexts, then holds ordinary private allocations until
real allocation failure, first in 1 MiB chunks and then individual pages. It
expects NO_MEMORY from allocation and CREATE, releases the held backing and
retries creation with the same contexts. This deliberately consumes available
guest backing, can take time, and supplies no fault injection. Other processes
can experience allocation failure during the run. A different actual failure or
a successful exhausted CREATE is reported as a failed qualification.

`stop` and `fault` launch a separate copy with two live siblings. That copy hands
both completion observers to its supervisor. The supervisor either requests
process termination or permits a sibling's deliberate UD2, then requires both
thread results to be STOPPED and the process to be TERMINATED or FAULTED after
reclamation. Fault details remain in the kernel log.

`close` receives a sibling's endpoint CALL to establish admission, attempts
release of its pinned stack, then closes the shared caller slot and receiver.
It requires the admitted CALL to unwind with ENDPOINT_CLOSED, completion to
follow retirement, and subsequent reservation release to succeed. This checks
one bounded interleaving, not every device or capability race.

Siblings enter a small assembly trampoline, use native wrappers, atomics and raw
FS assembly, and EXIT explicitly. They never rerun process startup or use shared
libc allocation, stdio or errno. Parent printing happens before sibling creation
and after completion. These manual probes do not advertise thread-safe libc,
pthreads, C11 threads, address parking or language TLS.
