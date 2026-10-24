# Git

Optional first-party CLI for public HTTPS clone, log and status, using go-git
v5.19.3 and go-billy v5.9.2. Run log/status at the repository root. Clone creates
a fresh child of the selected cwd and retains partial destinations on failure.
Symlinks become regular target-text files; Git index/tree modes remain intact.

```sh
./git/build.sh /absolute/pyxis-go-prefix /absolute/fresh/git-build
./git/build.sh /absolute/pyxis-go-prefix /absolute/fresh/git-linux linux
```

The selected installed Go compiler builds amd64 Pyxis or Linux from the same
CLI and patches. Sources and caches are private to OUT. Dependencies download
only through the internal Go proxy, using the committed module/checksum pins.
Vendoring must preserve those files. Patches apply in filename order; target
compilation is offline. No compiler rebuild or vendor tree is committed here.

Pyxis stages at `OUT/stage/bin/git.pxb`; Linux stages at
`OUT/stage-linux/bin/git`. Bundle metadata retains complete upstream notices
for compiled modules, their original checksums, the applied patches, source
hashes, userland/Go licenses and compiler revision. Use a fresh OUT for staging.

The manifest requires memory, threads, precise clock and random. TCP CONNECT,
DNS and service/create are optional so offline log/status can run without
network authority. The caller supplies cwd/storage and read-only boot trust
roots; the bundle does not mint those rights. There is no LISTEN grant.
