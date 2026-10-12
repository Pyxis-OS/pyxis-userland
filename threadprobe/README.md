# Native thread qualification

Build with a selected SDK exporting native threads, guarded memory and the static
TLS runtime, using its matching TLS-capable compiler and rebuilt C++ archives:

```sh
make SDK=/path/to/sdk threadprobe
```

Run manually from an interactive shell. `session` delegates the `threads`
resource; ordinary foreground and background launches do not. Each invocation
replaces the shell and needs a fresh session afterward.
Local mux pane shells and remote terminal shells retain the optional native
service for this explicit session delegation.

```text
session bin://threadprobe.pxe lifecycle
session bin://threadprobe.pxe inspect
session bin://threadprobe.pxe tls
session bin://threadprobe.pxe tls-inspect
session bin://threadprobe.pxe tls-stop
session bin://threadprobe.pxe tls-fault
session bin://threadprobe.pxe bind
session bin://threadprobe.pxe refusal
session bin://threadprobe.pxe exhaust
session bin://threadprobe.pxe stop
session bin://threadprobe.pxe fault
session bin://threadprobe.pxe close
session bin://threadprobe.pxe park32
session bin://threadprobe.pxe park64
session bin://threadprobe.pxe park-refusal
session bin://threadprobe.pxe park-inspect
session bin://threadprobe.pxe park-unmap
session bin://threadprobe.pxe park-stop
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

`bind` attempts two bindings of raw FS contexts after libc startup has already
bound the initial thread. Both must return BUSY; both unbound raw contexts are
released. `refusal` checks pinned
release, duplicate live stack/FS identities, stale identity and NX entry refusal;
successful subsequent creation checks provisional pin unwind. Raw sibling FS
contexts contain only a marker, and do not support libc TLS or errno.

`tls` prepares two libc contexts with independent guarded 64 KiB stacks and
native CREATE entries. Parent and siblings require the executable's initialized
C TLS and zero tail, then keep distinct values and addresses across the sibling
hold. Initialized TLS requests 8192-byte alignment; errno begins at zero in each
TCB and keeps separate parent/sibling values. The C++ helper initializes two real
`thread_local` objects in order and records destructor order `21` at ordinary
exit. Each runtime-key callback observes its cleared value, verifies language
cleanup has finished and reassigns itself even on its fourth call; exactly four
calls must occur. Invalid and deleted keys refuse; a replacement key cannot see
or overwrite a stale generation's value. Deleting keys never calls their
callbacks. The parent checks native BUSY for live context/stack release, then
uses libc collection, repeats completion WAIT and releases the stacks.

`tls-inspect` holds siblings until Enter. Before releasing them, use `tls_contexts`
and the printed backing/FS addresses with the matching debug ELF to inspect
shared CR3, distinct RSP/FS, FS:0 self pointers, negative local-exec TLS offsets,
8192-byte alignment, errno addresses and unmapped lower guards. User observations
alone do not establish CR3 or CPU placement. Repeat on one and four CPUs. This
executable's over-aligned TLS cannot qualify a template whose maximum alignment
is only four bytes; the separate compiler/runtime qualification must cover it.

`tls-stop` and `tls-fault` reuse the existing supervisor with prepared libc
contexts, initialized C++ objects and key values. They require STOPPED sibling
completions and TERMINATED/FAULTED process completion. Use GDB breakpoints on
`threadprobe_tls_note` and `tls_key_callback` to qualify the absence of user
callbacks: completion kinds alone cannot establish that property. No live
context is released before completion.

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

`park32` and `park64` put two siblings on one atomic word for 64 rounds. Each
round requires a positive native one-waiter wake count while the predicate is
unchanged, then advances the predicate and wakes remaining registrations. The
parent requires every round to finish within five seconds, checks returned wake
counts against the siblings' observed OK results, joins both and requires an
empty final wake. A readiness flag alone is not treated as proof of enrollment.
The wake with an unchanged predicate exercises the required predicate recheck.
These two modes poll wake enrollment and round completion with native clock
reads rather than a fixed sleep, allowing wake before physical parking. Use GDB
to establish that early-notification path; scheduling alone does not prove it.

`park-refusal` checks writable .bss, stack and ordinary heap words, mismatch,
zero-deadline polling, future 20 ms absolute deadlines, empty wake and unaligned,
unmapped, read-only and invalid 32-bit expected-value refusals. Heap backing is
released after expiry. Failed wake counts must be cleared. Use GDB at the
native wait and after its timed return to inspect timer/bucket detachment and
zero mapping borrowers; successful user results alone do not prove that state.

`park-inspect` holds two 64-bit waiters until Enter. Before releasing them, use
the printed word/context addresses and matching debug ELF to inspect both bucket
registrations, their mapping incarnations, task waits and zero mapping borrowers.
`park-unmap` similarly requires GDB to establish its one waiter's enrollment
before Enter: releasing its separate ordinary RAM page must return BAD_BUFFER
to WAIT. It allocates a new page and wakes it before collecting the old result,
requiring zero old registrations. The summary reports whether the address was
reused; only a reused address exercises that ABA case. Allocation placement is
not promised by the probe. Do not release before verifying the waiter is parked:
an earlier user load racing unmap can legitimately fault instead.

`park-stop` reuses the existing process supervisor with two native parking
siblings. Before transferring their observers, the child requires each sibling
to have consumed an actual wake with its predicate still false. Termination must
yield STOPPED for both siblings and TERMINATED for the process. Before stopping
the child, the supervisor wakes its own word at the same ELF virtual address and
requires an OK result with zero registrations, checking process-key isolation.
The isolation result prints after child/thread completion. Inspect bucket
and timer detachment with GDB; the probe adds no timer, output or boot automation.

Raw siblings enter a small assembly trampoline, use native wrappers, atomics and
raw FS assembly, and EXIT explicitly. TLS siblings instead use the prepared libc
context, errno, runtime keys and language TLS destructor registration, finishing
with `libc_thread_exit`. Siblings never rerun process startup or use shared libc
allocation or stdio. Parent printing happens before sibling creation and after
completion. These manual probes qualify the narrow TLS runtime; they do not
advertise thread-safe libc/C++, pthreads or C11 threads. Parking modes require the SDK/kernel address
WAIT/WAKE gate; there is no cross-process wait, requeue or lock-ownership handoff.
