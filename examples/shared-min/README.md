# Shared buffer example

Build explicitly against an SDK containing the native shared-memory helpers,
`pyxis/shared_memory.h` and contained native jobs:

```sh
make SDK=/path/to/sdk shared-min
```

This produces `build/apps/shared-min.pxb`. Run the bundle itself with stdin,
for example `./shared-min.pxb < input.txt`. Its manifest requires explicit
`memory/manage` and `memory/create_shared`, `service/create`, `launcher/launch`
and `clock/read`. The launcher must hold and delegate those declared grants;
ordinary MEMORY MANAGE does not grant creation. Normal bundle stream bindings
supply stdin, stdout and stderr. Success prints `bytes=... lines=...`, where
lines counts newline bytes, after actual worker retirement and cleanup.

The parent uses the libc shared-memory extensions to create a region containing
a 64 KiB data buffer and an atomic sequence word. These sizes are example
implementation choices. It holds the native entry FILE selected once from its
bundle's `app/bin/shared-min.pxe` and passes that same FILE to a contained leaf
launch; it never resolves `argv[0]` to choose code. Keep the published bundle
unchanged while running it.

The worker receives only MEMORY MANAGE, a READ-only region grant, an endpoint
CALL client and stderr. It has no stdin, launcher, endpoint receiver or creation
right. It maps the region with the native shared-memory helper. Its initial CALL
announces readiness; later CALLs return the previous span's byte/newline counts.
The parent acknowledges that CALL with the next span or STOP. Before replying,
it fills the buffer and release-publishes the sequence; after receiving that
reply, the worker validates one acquire-load against the expected sequence
before reading. A mismatch fails the worker without spinning or retrying.
Count metadata travels through the endpoint while input bytes stay in the shared buffer. Acknowledgment precedes each reuse, so
input may span any number of buffers without a fixed transport-size limit.
Unrepresentable totals fail instead of wrapping.

Only the parent owns the receiver. It watches receiver readiness together with
actual child completion, using repeated bounded waits and CLOCK READ; these
waits impose no overall stream timeout. The worker CALL has no deadline, so
waiting for slow stdin is permitted.

EOF sends STOP, then the parent joins the actual process and contained group
before releasing its view and region. Input, worker or protocol errors close
the receiver, request subtree termination and observe cleanup before release.
Admission and data replies are never replayed after an unknown outcome. WAIT
may observe cleanup again after cancellation; no process result is invented.
A returned failed-launch job record remains owned through terminate/wait/close.
If cleanup cannot be confirmed, the buffer is left to process teardown rather
than explicitly released while a worker may still use it. Handle close alone
does not unmap either process's view. Input/output or cleanup failures exit
nonzero.

The default installation includes `share/examples/shared-min.c` and
`shared-min-manifest.json` beside the other SDK example sources. The compiled
bundle is opt-in and is excluded from the default build and installation.
