# Git

Optional first-party CLI for public HTTPS clone, log and status, using go-git
v5.19.3 and go-billy v5.9.2. Run log/status at the repository root. Clone creates
a fresh child of the selected cwd and retains partial destinations on failure.
Symlinks become regular target-text files; Git index/tree modes remain intact.

`git clone HTTPS_URL [DIRECTORY]` refuses existing destinations; `git log [-n
COUNT]` and `git status` run at the repository root. No parent discovery, `.git`
pointer files, alternates, global configuration, helpers or non-HTTPS transport
are admitted. Log/status do not write. First Ctrl+C requests cancellation;
transport observes it, while checkout can finish an in-flight operation before
reporting cancellation. There is no whole-clone deadline or memory bound.

Writable files sync on close, and publication syncs the source before its
same-store rename. Ref locks use exclusive creation. This is data-before-name
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
hashes, userland/Go licenses and compiler revision. Use a fresh OUT for staging.

The manifest requires memory, threads, precise clock and random. TCP CONNECT,
DNS and service/create are optional so offline log/status can run without
network authority. The caller supplies cwd/storage and read-only boot trust
roots; the bundle does not mint those rights. There is no LISTEN grant.
