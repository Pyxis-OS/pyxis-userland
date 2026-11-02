# Boot manager

`bootmgr` reads and edits the firmware's UEFI boot variables through the typed
`uefi` resource's boot-manager right. A space receives it only by setting
`boot_manager = true`; installed systems set it on `pyxis`'s local shell, and
remote shells never receive it. The program needs no other authority.

```text
bootmgr                          BootCurrent, BootNext, Timeout, BootOrder and every entry
bootmgr next 0003                boot entry 0003 once, on the next restart
bootmgr next --clear
bootmgr order 0003,0001          set BootOrder (hexadecimal numbers, as Boot####)
bootmgr timeout 5 | --clear
bootmgr create Fedora --like 0003 --file /EFI/fedora/shimx64.efi [--inactive] [--first]
bootmgr edit 0003 [--label NAME] [--file PATH] [--active | --inactive]
bootmgr delete 0003              deletes Boot0003 and removes it from BootOrder
```

A new entry copies the device path of the `--like` entry (the partition) and
replaces its file path; this program has no disk or partition view, so it cannot
describe a partition the firmware does not already name. Every write is read
back by the kernel. A write that could not be confirmed reports so honestly
(`OUTCOME_UNKNOWN`) and nothing is retried: run `bootmgr` to see the variables
before changing anything else. There are no safeguards against a bad boot
order or a deleted entry.
