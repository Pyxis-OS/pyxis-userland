# cpuclock

Portable Linux/Pyxis x86-64 dependent-add rate probe, not an MSR clock reader.

```sh
cpuclock --threads 1 --samples 5 --milliseconds 1000
cpuclock --threads 16 --samples 5 --milliseconds 1000
```

Pass the actual CPU allowance as N for one worker per offered CPU. No CPU-count
query or affinity grant is added: Pyxis's threads/clock interfaces cannot report
that allowance, and global online count is not process affinity. Threads inherit
the caller's CPU set and may migrate. Worker indices are not CPU indices.
Defaults are one thread, five samples and 1000 ms. Workload limits are 64 workers,
1000 samples and 60000 ms per sample. Main participates as worker zero, so 64
workers fit the current C11 record limit including main; fewer may be available.
Thread creation failure refuses the measurement and joins known workers.

Each sample uses a fresh common monotonic deadline after a blocking C11 barrier.
Workers run 1,048,576 counted dependent integer ADDs per batch (256 in volatile
assembly, repeated 4096 times), checking CLOCK_MONOTONIC between batches. A live
register accumulator and early-clobber constraint prevent folding or operand
aliasing. A final checksum checks modulo-2^64 arithmetic consistency; optimized
disassembly establishes the ADD instruction sequence.
The actual elapsed time includes the final batch overshoot. Start skew is
reported; a worker that first runs after the deadline is invalid, not zero GHz.
Workers persist across samples; output is deferred until all measurements end.

GHz = adds / elapsed_nanoseconds **assumes one dependent register ADD per cycle**.
Loop/clock overhead, descheduling, interrupts, SMT contention and migration lower
this wall-time rate. It is not proof of a CPU's hardware frequency. Calibrate the
assumption on horse against an independently observed under-load frequency;
record that separately. No CPU/MSR/power/scheduler writes or privileged grant.

Pyxis requires ordinary memory MANAGE, threads MANAGE and clock READ|PRECISE.
Untimed conditions use native thread parking, not clock sleeps. No system_info,
network, disk, launcher, random or power grant is requested by the bundle.

Native build uses the parent userland target:

```sh
make -C /absolute/pyxis-userland SDK=/absolute/sdk BUILD=/absolute/out cpuclock
```

The separate Linux build needs only a normal x86-64 C compiler/C11 threads:

```sh
make -C cpuclock BUILD=/absolute/linux-out
/absolute/linux-out/cpuclock --threads 1
```

No new framework, kernel/ABI or compiler-container rebuild. Native package
integration belongs to Pyxis. Keep the complete output for every worker/sample;
the final PASS/FAIL line summarizes the range. Failure, invalid timing or output
error returns nonzero. A failed observation keeps shared state alive until
process exit rather than freeing it under a possibly live worker.
