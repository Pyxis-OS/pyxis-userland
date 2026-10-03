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
Connected root devices show physical port, speed and numeric vendor/product IDs,
with optional database names. Controllers follow boot registry order, stable for
the boot but otherwise unspecified; PCI-address sorting is not guaranteed.
Unidentified devices say `unidentified` without invented IDs. Every checked
interface shows configuration value, interface number, alternate setting, numeric
class/subclass/protocol and endpoint count. These are observed descriptors;
output does not imply that a configuration is selected or an interface bound.

`-n` skips the name database. `-i FILE` replaces
`app://share/hwdata/usb.ids`. The parser streams one pass with at most 1023 bytes
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

The snapshot is immutable for the boot. Later removal or failure does not update
it. It has no device strings or serials, hub descendants, transfer operations,
rescan, hotplug or device ownership authority. A hub is labeled with unavailable
descendants. An empty complete snapshot can legitimately print no controllers.
