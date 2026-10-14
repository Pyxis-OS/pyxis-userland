# Run-sheet checks

Small Lua scripts for checks that a person runs by hand, usually from a native
run sheet. The image installs them as `boot://share/checks/`. Run one with `lua`
and its URI; Lua scripts do not start through a shebang:

```text
lua boot://share/checks/console-burst.lua
lua boot://share/checks/console-flood.lua 8
```

Arguments are positional. Without arguments, each script runs the workload of the
native sheet it came from. Each script ends with one summary line: its name, then
`key=value` fields. That line is for reading, a photo or `grep`. It is not a
stable interface: fields may change with the script, so a sheet names the
revision it ran. The scripts write no files. Use shell redirection to keep their
output.

| Script | Arguments (defaults) | Measures | Summary fields |
| --- | --- | --- | --- |
| `console-burst.lua` | writes (40, at most 100000), lines per write (256, at most 65536) | Writes runs of empty lines to standard output, flushing after each run. With 256 lines or fewer, each run is one console write. | `writes`, `lines`, `us_per_write` (mean) |
| `console-flood.lua` | seconds (8, at most 3600) | Prints numbered lines, flushing after each line, for the period. | `seconds`, `lines`, `lines_per_s` |
| `exit-latency.lua` | launches (100, at most 100000), seconds (20, at most 3600) | Launches `bin://echo.pxe -n` with `pyxis.run` and times each launch through its exit. Stops at the launch count or the time limit. | `launches`, `median_us`, `p90_us`, `max_us` |
| `cpu-busy.lua` | seconds (24, at most 3600) | Spins on `pyxis.monotonic_ns` for the period. Start several in the background to keep CPUs busy; their iteration counts show user progress. | `seconds`, `iterations` |

Console scripts measure the terminal they run in. The local spaces render
through the kernel, so their results differ from the Remote space, whose output
travels over the network. `exit-latency.lua` includes the kernel's release of
each exited process, because `pyxis.run` returns after its WAIT completes.

The scripts need only the `lua` interpreter and its `pyxis` module. An invalid
argument prints a usage line on standard error and exits with status 1 before
any work starts.
