# lsusb

Native USB inspection through the startup `system_info` READ grant:

```text
lsusb
lsusb -n
lsusb -i host://usb.ids
lsusb | cat
```

Build with `make -j16 SDK=/path/to/sdk lsusb`. The normal install includes
`lsusb.pxe`. The SDK must contain the USB observation ABI and current libpyxis
helpers; runtime changes require SDK assembly before application builds.

Each controller shows its PCI address, numeric IDs, state and physical root-port
count, including unsupported controllers. An unavailable root-port count is
shown as `unknown`; a retained nonzero count is shown even for failed controllers.
Speeds include distinct `super` and `super-plus` link categories without numeric
rate or lane-count claims. Connected devices show their physical port path, speed
and numeric vendor/product IDs, with optional database names. For example, `Port 3.2.1` identifies root port 3,
downstream port 2 of its hub, then downstream port 1 of the next hub. The components
are full one-based physical port numbers, not compressed controller route values.
Controllers follow boot registry order, stable for the boot but otherwise
unspecified; PCI-address sorting is not guaranteed. Within each controller,
devices retain snapshot order: roots first, then breadth-first descendants.
Unidentified devices say `unidentified` without invented IDs. Every checked
interface shows configuration value, interface number, alternate setting, numeric
class/subclass/protocol and endpoint count. These are observed descriptors;
output does not imply that a configuration is selected or an interface bound.

`-n` skips the name database. `-i FILE` replaces
`boot://share/hwdata/usb.ids`. The parser streams one pass with at most 1023 bytes
per record and retains labels only for listed device IDs. Vendors and one-tab
products are used; two-tab interface lines and class/other sections are ignored.
Malformed, oversized or embedded-NUL records supply no names. First labels win.
Database bytes outside printable ASCII become `\xHH`; backslashes become `\\`.
Names are descriptive data, never device identity or authority.

A complete inventory exits 0. Missing or unreadable databases, including name
allocation failures, give a stderr note and numeric output without changing
inventory success. Unknown names retain numeric IDs. A database failure discards
all partially loaded names. An incomplete snapshot prints retained observations
and exits 1. Unavailable or initializing snapshots, missing authority, invalid
replies, failed queries, bad usage and stdout errors exit 1.

Device and interface ranges and reserved fields are checked before printing.
A root has no parent and a zero parent port; each descendant names an earlier hub
on the same controller and root port, with a nonzero downstream port. These
relations also make walking a port path finite without an application depth limit.

The snapshot is immutable for the boot. Later removal or failure does not update
it. Checked USB2 hub descendants are included. A hub is labeled `(hub)`;
`(incomplete)` marks a partial device observation, including descendants that
could not be inspected. A hub rejected before capturing its ports appears as an
incomplete hub without child records. The snapshot has no device strings or
serials, transfer operations, rescan, hotplug or device ownership authority. An
empty complete snapshot can legitimately print no controllers.
