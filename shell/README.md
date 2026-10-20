# Pyxis shell

`shell PATH [argument...]` runs a native `.pxs` script through this bundle's
declared grants, and `shell -c "LINE" [argument...]` runs one command line the
same way; Neovim's `'shell'` uses it. Console input/output, threads, random and
the retention account and a private job provisioner are optional grants:
without a retention account `$(...)` captures are refused. Where the caller
can provision jobs, a foreground launch receives its own private provisioning subtree;
`FILE`, `-c` and nested exact-shell scripts can launch declared job clients.
Typed captures retain capabilities: `api=$(http prepare POST URI)`, then
`reply=$(printf '{}' > $api)` and `cat $reply`. Ctrl+C stops the script subtree.

Pipelines bind tighter than `&&` and `||`, which evaluate left to right. Only
selected commands expand and launch. Scripts and `-c` stop after a failed
conditional list; `;` starts the next list. `exit [N]` accepts a signed 32-bit
decimal status and defaults to zero.

Foreground external commands accept ordered `2>&1`: `cmd >file 2>&1`
merges both outputs, while `cmd 2>&1 >file` keeps stderr on the original stdout.
Pipes and consoles share their endpoint; merged files and captures use one byte
relay/cursor, including through descendants. A redirected FILE output is truncated
once, unless appended to, and then passed with append policy, so a script's commands
follow each other. Explicit and inherited append are
preserved without atomic-append guarantees. Merged streams cannot seek or hand
off capabilities. Relay workers share the eight-process batch bound; their
failure fails the command with status 1 without replacing user fault/termination
outcomes, and descendant writers must close before completion. Direct console
and pipe merges keep the ordinary command lifetime. Captures without `2>&1`
keep typed handoff. A captured transaction response requires an empty collector;
`$(cmd 2>&1 >$api)` is refused before redirect OPEN because stderr bytes would
conflict with response handoff.

Nested scripts with the exact `#!boot://init-shell.pxe` interpreter use the
bundle's held `app/bin/shell.pxe` image and forward only held pipe creation,
subgroup-bound child launch and optional threads. When explicitly available,
foreground launch preparation also creates a fresh private job provisioning subtree for
the selected interpreter. Ordinary programs receive no implicit job context.
The image stays private and
carries no interpreter trust; unavailable images refuse re-entry. Mutable
scheme bindings cannot replace it.

Interactive shells started by session, mux and Remote apply
`home://.config/shell/startup.lua` from a private `shell_settings` READ FILE
(`settings.c`): strict literal TEXT settings only, validated in full before
one SET batch. See the shell reference's startup settings section.

Trusted startup uses the separately packaged `boot://init-shell.pxe` with a
pre-granted script input. This bundle neither grants root launch authority nor
creates retention accounts.
