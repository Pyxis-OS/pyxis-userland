# Git

Optional first-party CLI for HTTPS clone/push/pull and local init, log, status,
add, commit, diff and remote setup, using go-git v5.19.3 and go-billy v5.9.2. Clone creates a fresh
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
git init
git remote add NAME HTTPS_URL
git remote set-url NAME HTTPS_URL
git remote -v
git push [REMOTE]
git push -u REMOTE CURRENT_BRANCH
git pull [REMOTE]
```

Repository commands discover the nearest real `.git` along the retained cwd's
held ancestors; they never recover authority from descriptive paths. Explicit
add operands are relative to the invoking directory, while `add -A` covers the
whole tree. New ignored files are skipped, tracked edits/removals still stage,
and submodule changes are not staged. A new nested repository refuses rather
than being flattened into ordinary files. Diff compares worktree/index or, with
`--cached`, index/HEAD: unified text with three context lines, binary markers,
no colour/pager. Differences alone do not set an error exit status.

Commit requires literal identity settings in `home://.config/git/config`:

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

`init` creates an unborn `main` in cwd and refuses an existing `.git`. The
first nonempty commit has no parent. Remote changes hold `config.lock` through
read/modify/sync/rename. Push requires a configured upstream with the current
branch's name and sends only its checked commit, never other branches/tags.
An absent upstream refuses with a hint to use `push -u REMOTE CURRENT_BRANCH`.
That explicit form records the upstream under `config.lock` only after confirmed
remote success; rejected or uncertain pushes never install one.
Pull defaults to `origin`, hashes the tracked tree/index before and after fetch,
refuses divergence and untracked collisions, and finishes checkout/index before
publishing the compared branch. Failed checkout can leave partial files; it does
not silently reset them.

HTTPS reads optional exact-origin credentials from
`home://.config/git/credentials`:

```ini
[credential "https://git.internal"]
    username = YOUR_USERNAME
    token = YOUR_TOKEN
```

Push requires a matching entry. Clone/pull can remain anonymous. Tokens are
plaintext home data; this is no credential-manager or Unix-permission guarantee.
Host case and omitted port 443 normalize; aliases, subdomains and other ports
do not inherit credentials. Authenticated redirects stay on that origin.
Credentials never go in URLs or repository config; errors discard remote body
and parser text. An uncertain push is reported without automatic retry.

The same application config admits explicit additive CA trust, separately from
credentials, for example after `[user]`:

```ini
[http "https://git.internal/"]
    sslCAInfo = home://.config/git/homelab-ca.pem
```

The selected regular PEM file is read through a held named root, augments the
public roots only for that exact origin and never disables TLS verification.
Missing/malformed selected files refuse. Other origins keep public roots. No
system-wide trust-store mechanism is supplied.

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
