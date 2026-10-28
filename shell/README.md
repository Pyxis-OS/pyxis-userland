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

Interactive foreground commands and pipelines use an owned execution subgroup.
The shell waits for direct stages and actual subtree retirement before returning
input to its prompt, including when a stage faults or exits before descendants.
For ordinary interactive commands, the first Ctrl+C acknowledges one press and
requests cooperative interruption of unfinished direct stages. Stages without a
receiver are terminated immediately. After the direct stages retire, the shell
stops remaining descendants and waits for actual subtree cleanup. A second press
forces the whole subgroup to stop immediately. There is no grace timer; actual
stage completion/status is preserved and cancellation still ends the command list.
Programs holding input passthrough continue to receive Ctrl+C as data.

Existing script, capture and output-helper scopes retain their immediate
whole-subtree stop policy. Noninteractive direct stages outside an owned scope
retain first-press cooperative interruption and second-press forced stop.

After a started interactive scope retires, the shell uses a separately held
terminal output grant to leave the alternate screen, reset the scroll region and
styles, show the cursor and start a fresh line. Redirected FILE/PIPE streams never
receive these controls. Failed or uncertain cleanup prevents normalization and
prompt return; normalization failures are reported as fatal. Descendant launcher
grants remain confined to the subgroup without adding launch authority; selected
ancestor or unrelated launchers are refused by the existing group grant policy.
Session handoff and background command lifetimes keep their existing ownership.

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
and pipe merges follow the same foreground subgroup lifetime. Captures without `2>&1`
keep typed handoff. A captured transaction response requires an empty collector;
`$(cmd 2>&1 >$api)` is refused before redirect OPEN because stderr bytes would
conflict with response handoff.

Scripts selecting the exact held Pyxis shell inherit its launch-source inventory,
including JIT when held, in foreground, pipelines, background and captures. Child
bundles still select declared grants; explicit reduced shell bundle/native launches
stay reduced. Script sources are not a sandbox: use a restricted space when one
is needed. Streams, scoped launchers and job-directory cleanup retain their own
lifetimes. The script helper adds only the script READ FILE and argv, not a list
of runtime extras. Held interpreter identity is independent of mutable scheme
bindings; rootless shells retain their own image without gaining a trusted root.

Interactive shells started by session, mux and Remote apply
`home://.config/shell/startup.lua` from a private `shell_settings` READ FILE
(`settings.c`): literal text settings and literal argument-array aliases,
validated and prepared before the environment SET batch. Aliases stay private
and replace one plain command word once, including builtin names; quoting or
an explicit path bypasses them. Configured arguments are never parsed as code.
Tab offers aliases and completes explicit command paths with the existing path
completer. See the shell reference's startup settings section.

Trusted startup uses the separately packaged `boot://init-shell.pxe` with a
pre-granted script input. This bundle neither grants root launch authority nor
creates retention accounts.

A Ctrl+C-cancelled foreground job ends its command list, even if an opted-in
process exits gracefully. Its actual completion kind and status are preserved.
