# FAT32 controller utility

`fat32ctl.pxb` requests only declared grants from selected sources. `list` uses
the optional `fat_mounts` inventory source with enumeration alone. `state` and
`retire` use the optional `fat32_control` source with OBSERVE/RETIRE; `sync`
separately requests WRITE from that source. Missing grants and native refusals
are reported. The runtime also requires its ordinary MEMORY grant. The utility
prints the actual received controller rights.
`state` also reports invalid RW allocation proof separately from a failed writer.

```text
fat32ctl list
fat32ctl state
fat32ctl sync
fat32ctl retire
```

This child cannot change its parent shell's bindings or close the parent's data
handles. Retirement remains BUSY while those views or other references remain.
Use the shell's `release` builtin to unbind and drain its own references. Closing
the utility never retires, synchronizes or revokes the mount implicitly.
