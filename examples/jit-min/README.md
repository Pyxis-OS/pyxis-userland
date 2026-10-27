# JIT minimum example

Build explicitly against an SDK containing the JIT ABI and libpyxis wrappers:

```sh
make SDK=/path/to/sdk jit-min
```

This creates `build/apps/jit-min.pxb`. Copy that bundle into Pyxis and run
`./jit-min.pxb` from a shell in a space created with `jit = true`. The gate is
fixed when the space is created; changing configuration requires a new boot or
space. No shipped space enables it by default. Launch the bundle itself: its
manifest explicitly requests `jit/manage`, while an ordinary executable launch
receives no implicit JIT grant. Standard output and error use the launcher's
existing bundle stream bindings.

The example allocates two pages, puts x86-64 code on the first and the constant
42 on the second, then protects constants R/NX and code R/X. It calls the code,
withdraws execute permission before rewriting it, republishes it R/X and calls
again for 43. It prints PASS only after successful release. This is a single
thread example; callers must exclude other logical code users before rewriting
or releasing generated code.

The installed source examples include `boot://share/examples/jit-min.c` and
`jit-min-manifest.json`. They may also be built with hosted Clang into the same
`jit-min.pxb/app/bin/jit-min.pxe` layout. The compiled example is excluded from
the default build and installation.
