# Pyxis shell

`shell PATH [argument...]` runs a native `.pxs` script through this bundle's
declared grants, and `shell -c "LINE" [argument...]` runs one command line the
same way; Neovim's `'shell'` uses it. Console input/output, threads, random and
the retention account are optional grants: without a retention account `$(...)`
captures are refused.
Typed captures retain capabilities: `api=$(http prepare POST URI)`, then
`reply=$(printf '{}' > $api)` and `cat $reply`. Ctrl+C stops the script subtree.

Pipelines bind tighter than `&&` and `||`, which evaluate left to right. Only
selected commands expand and launch. Scripts and `-c` stop after a failed
conditional list; `;` starts the next list. `exit [N]` accepts a signed 32-bit
decimal status and defaults to zero.

Nested scripts with the exact `#!boot://init-shell.pxe` interpreter use the
bundle's held `app/bin/shell.pxe` image and forward only held pipe creation,
subgroup-bound child launch and optional threads. The image stays private and
carries no interpreter trust; unavailable images refuse re-entry. Mutable
scheme bindings cannot replace it.

Trusted startup uses the separately packaged `boot://init-shell.pxe` with a
pre-granted script input. This bundle neither grants root launch authority nor
creates retention accounts.
