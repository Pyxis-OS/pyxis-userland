# Network controller listing

`net.pxb` lists the network controllers and the net0 state through the optional
`net_config` grant, which it declares with READ alone. A shell supplies that
grant only to a bundle that declares it. Without it, `net` reports that the
grant is missing. It changes nothing.

```text
net
net --help
```
