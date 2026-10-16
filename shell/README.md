# Pyxis shell

`shell PATH` runs a native `.pxs` script through this bundle's declared grants.
Typed captures retain capabilities: `api=$(http prepare POST URI)`, then
`reply=$(printf '{}' > $api)` and `cat $reply`. Ctrl+C stops the script subtree.

Trusted startup uses the separately packaged `boot://init-shell.pxe` with a
pre-granted script input. This bundle neither grants root launch authority nor
creates retention accounts.
