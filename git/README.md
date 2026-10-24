# Git

Optional first-party CLI for public HTTPS clone and local log, status, add,
commit and diff, using go-git v5.19.3 and go-billy v5.9.2. Clone creates a fresh
child of the selected cwd and retains partial destinations on failure. Symlinks
become regular target-text files; Git index/tree modes remain intact.

```sh
git clone HTTPS_URL [DIRECTORY]
git log [-n COUNT]
git status
git add [--] PATH...
git add -A
git diff
git diff --cached
git commit -m MESSAGE
```

Repository commands discover the nearest real `.git` along the retained cwd's
held ancestors; they never recover authority from descriptive paths. Explicit
add operands are relative to the invoking directory, while `add -A` covers the
whole tree. New ignored files are skipped, tracked edits/removals still stage,
and submodule changes are not staged. A new nested repository refuses rather
than being flattened into ordinary files. Diff compares worktree/index or, with
`--cached`, index/HEAD: unified text with three context lines, binary markers,
no colour/pager. Differences alone do not set an error exit status.

Commit requires `home://.config/git/config` with only literal identity settings:

```ini
[user]
    name = Chronium
    email = chronium@example.invalid
```

Both author and committer use these values and checked native wall time with
the configured timezone. Missing/invalid identity or unavailable time refuses.
The Linux reference reads `.config/git/config` under its selected HOME. No
configuration-writing command, editor, amend, automatic staging, empty commit
or signing is supplied. Detached HEAD and unresolved indexes refuse commit.

No `.git` pointer files, alternates, global configuration, helpers or non-HTTPS
transport are admitted. Log/status/diff do not write. First Ctrl+C requests
cancellation; an already blocking native file call remains owned until actual
completion. There is no whole-operation deadline or memory bound.

Writable files sync on close, and publication syncs the source before its
same-store rename. Index locks span read/modify/publication; branch publication compares the
observed reference. Ref and index locks use exclusive creation. This is data-before-name
publication, without a directory-sync or whole-clone crash-atomicity claim.
Forced termination can leave partial files and locks; neither is stolen or
automatically retried. Unavailable timestamps force hashing; incomplete content
or ignore scans fail instead of reporting a clean tree.

```sh
./git/build.sh /absolute/pyxis-go-prefix /absolute/fresh/git-build
./git/build.sh /absolute/pyxis-go-prefix /absolute/fresh/git-linux linux
```

The selected installed Go compiler builds amd64 Pyxis or Linux from the same
CLI and patches. Sources and caches are private to OUT. Dependencies download
only through the internal Go proxy, using the committed module/checksum pins.
Vendoring must preserve those files. Patches apply in filename order; target
compilation is offline. No compiler rebuild or vendor tree is committed here.
Dependency patches retain the upstream Apache-2.0 terms. Their source pins are
go-git `bdae57cc427f94a6f7c627201bfe14508b5e19bd` and go-billy
`19ea4dd91ebc56e1b6cdea6890a180a789bea20d`.

Pyxis stages at `OUT/stage/bin/git.pxb`; Linux stages at
`OUT/stage-linux/bin/git`. Bundle metadata retains complete upstream notices
for compiled modules, their original checksums, the applied patches, source
hashes, userland/Go licenses and compiler revision. Repeated builds reuse OUT's
caches and replace the staged result only after compilation and notices succeed;
do not run concurrent builds into the same OUT.

The manifest requires memory, threads, precise clock and random. TCP CONNECT,
DNS and service/create are optional so offline log/status can run without
network authority. The caller supplies cwd/storage and read-only boot trust
roots; the bundle does not mint those rights. There is no LISTEN grant.
