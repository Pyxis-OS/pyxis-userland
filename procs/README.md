# procs

Lists the processes and execution groups of the space it runs in. With
`--all` it lists every space through the system process view, which the
bundle requests in its manifest.

```text
procs [--all] [--sort cpu|mem|name|age] [--live]
```

The default view is a tree: ungrouped processes, then each execution group with
its members. CPU % compares two listings 250 ms apart. `--sort` prints a flat
list instead. `--live` redraws every second; `c`, `m`, `n`, `a` and `t` change
the order and `q` or Ctrl+C exits.

Keys are per-boot listing keys, not process IDs. Names come from each
program's first argument and can be spoofed. MEM is committed private memory,
not RSS.
