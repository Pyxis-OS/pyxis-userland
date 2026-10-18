# Network listing

`net.pxb` lists retained controllers and native interfaces through the optional
`net_config` grant, which it declares with READ alone. Native rows include the
boot-local ID, class, MTU, address epoch and route revision; hardware and address
facts appear only when the kernel marks them valid. Loopback has no fabricated
MAC or carrier state. A shell supplies the grant only to a bundle that declares
it. Without it, `net` reports that the grant is missing. It changes nothing.

During the public-interface migration, the last `net0` row remains the existing
single-interface configuration/DNS snapshot. Native inventory names the same
physical context `eth0`; this checkpoint does not enable multiple configured
physical interfaces yet.

```text
net
net --help
```
