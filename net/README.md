# Network profiles and grants

Trusted boot-init reads `boot://config/network.lua` before starting spaces. A
profile is one complete ordered plan; Lua keys outside this schema are errors.
The packaged profile is:

```lua
return {
  wired = { auto = true, exclude = {} },
  interfaces = {},
  routing = { default = "eth0" },
  dns = { interface = "eth0", server = "1.1.1.1" },
  management = { remote = "eth0", log = "eth0", debug = "eth0" },
}
```

`wired.auto = true` covers every supported wired controller discovered at boot,
including carrier-down controllers. Explicit entries reserve their controllers
and names first. In automatic mode an explicit Ethernet `select = "link"` role
reserves its preferred prepared controller even while carrier is down/unknown;
MAC preference then controller ID fixes that initial choice. Its actual binding
still waits for known/up carrier. Explicit-only linked selection is unchanged.
Remaining controllers receive unused `ethN` names in ascending
boot-local controller-ID order, independent optional DHCP clients and the same
declared packet scopes. Bindings and scopes stay fixed until reboot; a new
post-boot adapter is not admitted by this mode. When inventory differs between
boots, use explicit MAC entries for roles that need a stable name.

`wired.exclude` is an array of nonzero unicast MAC strings. Excluded controllers
receive no automatic interface, DHCP client or packet scope. Selecting one
explicitly by MAC refuses setup; duplicate exclusion entries also fail validation.
Linked roles filter exclusions before preference/ID ranking, including late
binding. A required linked role with no permitted prepared candidate refuses
initial setup; a permitted candidate waiting for carrier can remain pending.
Exclusion does not disable the driver's mandatory receive draining. Omitting
`wired`, or setting `auto = false`, retains explicit-only configuration.

This first delivery does not rank routes or link speeds. The packaged route,
DNS and management choices remain pinned to `eth0`, even if another interface
has usable carrier or a lease. An absent automatic `ethN` policy pin means no
selected path (`eth0` through `eth253`), so an offline or all-excluded machine can
still boot locally;
there is no fallback. Undefined references remain errors in explicit-only
profiles. Automatic preference is a separate delivery.

`interfaces` is a consecutive array. Each entry has a unique canonical `ethN`
Ethernet name or `wlanN` Wi-Fi name and the corresponding `class = "ethernet"`
or `class = "wifi"`. `lo` already exists and is not an array entry. Names/IDs and
bindings remain retained until reboot. Interface IDs are boot-local and are
never recycled. This profile grants no Wi-Fi scan, association or credential
authority; that setup is separate from IPv4 configuration.

Exactly one selector is required: `driver = "virtio"`, `mac = "02:00:00:00:00:01"`,
or `select = "link"`. MACs must be nonzero unicast addresses. Selectors are
filtered by interface class. Driver/MAC selectors require a unique controller;
ambiguity or selecting the same controller twice refuses the whole binding
batch before associations change. Explicit-only link selection considers prepared
controllers with reported up carrier, chooses the lowest boot-local controller ID,
and can use
an ordered `prefer = { "02:00:00:00:00:01", ... }` list to rank MACs first.
Use distinct MAC selectors when multiple interfaces must select distinct NICs.
Unselected controllers stay visible; automatic wired mode covers those not
reserved by an explicit entry or excluded by MAC. Low-level driver draining is
independent of IP binding.

`optional = true` permits an absent explicit controller. Such interfaces remain
defined and may bind later through the trusted network owner; absence does not
block another client or the local shell. A link selector can remain pending
until carrier is reported. Binding is permanent; there is no runtime reassignment
or automatic fallback to another controller after a binding fails.

Use `dhcp = true`, or provide numeric `address`, integer `prefix` from 1 to 32,
and optional numeric `gateway` fields. For example:

```lua
{ name = "eth1", class = "ethernet", mac = "02:00:00:00:00:02",
  address = "192.168.50.2", prefix = 24, gateway = "192.168.50.1" }
```

DHCP and static address fields exclude each other. Static host/subnet/gateway
validation and duplicate static address/name checks run before network effects;
the kernel also rejects duplicate local addresses, including DHCP conflicts.
Connected prefixes may overlap. Configuration order breaks equal-prefix route
ties. Each DHCP client owns its own pinned port-68 broadcast endpoint, MAC,
transaction, retry state, lease and renewal/rebind/expiry timers. Active clients
share a bounded initial acquisition budget of ten seconds; a working interface
can hand off immediately. Maintenance continues independently after handoff. Healthy bound clients sleep
until the earliest owned renewal/expiry deadline. Pending setup, link waits,
active DHCP exchanges and cleanup retry at 100 ms, with peer expiry deadlines
still limiting the sleep.
Link/address/transport failure does not move a client to another interface.
Address-clear failures pause that client and retry its cleanup while peers keep
progressing. DHCP waits for carrier when it is reported; an otherwise ready
explicit controller with unknown carrier retains the driver's assumed-up behavior.

DHCP reports are timestamped userspace assertions, not kernel-verified lease
health. Initial replies remain broadcast-only and there is no independent
kernel lease-expiry backstop if the configuration owner stops running.

`routing.default` selects a retained interface explicitly. `dns.interface`
selects DNS independently: add `server` for a numeric resolver, or omit it to
use that interface's first DHCP DNS contribution. No lease changes either system
selection and no missing source selects another interface or resolver. Omitted
policy selections mean none. The packaged numeric `1.1.1.1` choice is explicit.

`management.remote`, `management.log` and `management.debug` select interfaces
independently of ordinary egress. Remote binds its listener using its retained
read facet and pinned factory. Reverse discovery captures the broadcast
endpoint's ingress interface and restricts CONNECT to that same ID. Its private
management factory is separate from the ordinary packet grants given to remote
shells/children. Address loss invalidates affected flows; reverse discovery may
retry on the same retained interface, while inbound listener recreation is not
provided. Unsupported/missing selections report unavailable with no fallback.

## Authority and launch delivery

`net_registry` READ enumerates controllers/interfaces, reads policy, and looks
up read-only interface facets. `net` never starts DMA, DHCP or scans. It prints
one interface row with retained identity, class/driver/MAC, binding/readiness,
carrier/authentication validity, IPv4/gateway and last reported DHCP phase.
Optional clock READ adds report age; route/DNS/management IDs come from the
read-only policy snapshot.

Only the explicit network-owner role receives private `config.NAME` interface
facets, `dhcp.NAME` pinned broadcast factories and `net_policy` administration
for late binding. Configuration facets cannot change another interface or system
route/DNS policy. Trusted init interpreters carry these declared resources across
SESSION handoff; ordinary programs, providers and successor local shells do not
receive them. Remote's `remote_interface` and `remote_tcp` are separate private
management resources carried only through its trusted setup.

Boot-init completes discovery within a ten-second setup deadline and resolves
the entire automatic plan before interface definitions or packet publication.
The kernel permits at most 254 non-loopback definitions; exceeding that bound
refuses setup. The network owner adopts the published interfaces using its
private configuration/DHCP grants and private `network.NAME` controller-ID hints
across the trusted SESSION handoff. These hints preserve an initial choice if a
binding is temporarily unavailable; text alone grants no configuration or packet
authority. The owner does not resample a new automatic inventory after handoff.

Stock `tcp`, `udp` and `echo` factories contain exactly `lo` plus the profile's
resolved initial IDs, including generated wired entries and any optional
interface not yet bound. Interfaces added
later never enter those held grants. Native `*_factory_derive` helpers return
one-interface factories with subset operation rights. Copies retain scope and
forwarder lineage. Closing a copied handle does not revoke other holders.
`*_path` operation helpers can additionally restrict an interface/source within
the held set; existing convenience helpers emit the same native wire format.

Trusted launches pair `DNS_SERVER` text with the declared `dns_udp` UDP OPEN
factory derived for the selected DNS interface from their permitted UDP scope.
The text grants no authority. A restricted scope that excludes that interface
has no resolver path; no wider packet grant is added. Registry-less successors
can retain their inherited resolver only through their held declared `dns_udp`.
New launches read the selected policy once; existing processes/providers retain
their startup choice. Numeric HTTP targets remain usable without DNS; hostname
requests report unavailable. Bundle/Lua grant roles must declare `dns_udp`
explicitly. Shell selected operands cannot replace an existing packet role with
a different grant: conflicting publication/source selections are refused.

## Private-profile migration

Legacy `net0` tables/booleans are rejected; no compatibility parser or implicit
second configuration authority remains. Before booting a private profile,
replace its single table with an `interfaces` array entry named `eth0`, add the
appropriate class, and move defaults/DNS/management choices into separate tables.
Keep its explicit selector, address fields or DHCP choice. A previous numeric
DNS choice becomes `dns = { interface = "eth0", server = "..." }`; use no server
field only when DHCP DNS is intended. Replace `net0 = false` with
`interfaces = {}` and omit route/DNS/management selections. A missing trusted
file also yields an empty plan with loopback-only stock packet scope.

The configuration editor shares this decoder. Unmigrated profiles fail validation
with an unknown `net0` setting; handwritten unsupported Lua remains untouched.
Review the explicit literal profile and configured-owner choices before applying
it. Runtime controller/radio availability is separate from static validation.
