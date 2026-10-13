# Scratch configuration publication

Build Pyxis with `make -j8 image INSTALLER_PUBLICATION_SCRATCH=1`. In the Install
boot entry, type `scratch` at the first prompt. This is a disposable-image
qualification path, not installed Update. Normal builds omit that entry and
retain the existing Install/Update and boot-loader behaviour. Never use this
path on native media.

Select the exact disposable disk ID and GPT ESP partition. Preparation opens a
native read-only FAT view, freezes the config/catalog/record/menu bytes and
issuer facts, inspects the personal pool override without mounting or replay,
then drains and explicitly retires the view. Review shows entry choices, source
selection, the retained override hash and the fresh boot-payload attempt. It
uses the same literal/schema/delta planner as `config`; unsupported Lua is data,
not executed code. `confirm` applies only that owned in-memory plan; `cancel`
writes nothing. The scratch flow installs no program revision.

The executor reacquires the existing whole-disk claim and rechecks the frozen
bytes with the shared FAT reader before staging. It releases that claim before
native FAT32 write admission, repeats the snapshots, and streams fresh kernel,
filtered rescue archive and revision files into
`EFI/pyxis/REVISION/ATTEMPT/`. The immutable ownership record certifies their
exact names, sizes and hashes. Files are synchronized and reread before the
exclusive configuration interval rechecks identity/content and publishes a
prepared, then committed, boot-bound readiness record. Only then may the exact
owned normal/rescue Limine entries move. Foreign bytes stay unchanged. Exact
legacy Pyxis menu adoption additionally requires a disk-bound recipe, validated
kernel/archive/revision structure and retained payload hashes rechecked under
the final interval. Ambiguous menus are printed for manual adoption and remain
unchanged: the result is `staged, boot activation pending`.

One previous boot pair is retained; older pairs are removed only after entry
confirmation and matching immutable ownership/hash evidence. Unknown ownership
skips cleanup. Failure or unknown completion stops at the reported phase with
artifacts retained; there is no retry, rollback, automatic repair or reboot
offer. Close is not sync or retirement. A cleanup failure does not report full
success.

The readiness reader accepts frozen bytes and actual kernel/program identities;
it has no issuer or writer authority. It chooses a complete compatible ESP
triple or the entire archive triple. Archive selection certifies no ESP files,
and rescue always selects archive. The current scratch frontend previews this
handoff without launching installed startup. No live ESP reference reaches a
configuration consumer. Catalog identity is independent of kernel/program
revision; the same generated catalog is packaged with trusted archive defaults
and the editor bundle.

Limits: configuration files remain 1 MiB, completion records 8 KiB, and each
boot payload 1 GiB. Streaming uses bounded native FILE calls. Publication uses
the existing non-extending 30-second interval. Small-volume success does not
clear the full-size FAT32 lease-cost gate; internal coexistence additionally
waits for partition-first NVMe authority. Installed activation and replacement
of destructive Update remain blocked, and external adversarial review precedes
release.
