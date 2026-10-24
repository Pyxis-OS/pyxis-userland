#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 GO_PREFIX OUT [pyxis|linux]" >&2
  exit 2
fi
go_prefix=$(realpath "$1")
mkdir -p "$2"
out=$(realpath "$2")
target=${3:-pyxis}
case "$target" in pyxis|linux) ;; *) echo "unsupported target: $target" >&2; exit 2 ;; esac
recipe=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname -- "$recipe")
source_repository=$(git -C "$repo" remote get-url origin)
source_commit=$(git -C "$repo" rev-parse HEAD)
go="$go_prefix/bin/go"
[[ -x "$go" ]] || { echo "missing Go compiler: $go" >&2; exit 1; }
[[ -f "$recipe/main.go" ]] || { echo "missing Git CLI source: $recipe/main.go" >&2; exit 1; }
for file in LICENSE PATENTS share/pyxis-go/revision; do
  [[ -f "$go_prefix/$file" ]] || { echo "missing compiler provenance: $file" >&2; exit 1; }
done

work=$(mktemp -d "$out/work.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
source_dir="$work/source"
mkdir "$source_dir"
cp "$recipe"/*.go "$recipe/go.mod" "$recipe/go.sum" "$recipe/README.md" \
  "$recipe/manifest.json" "$recipe/build.sh" "$recipe/stage-notices.py" "$source_dir/"
cp -a "$recipe/patches" "$source_dir/"
python3 - "$source_dir" "$work/source-sha256.txt" <<'PYHASH'
import hashlib
from pathlib import Path
import sys
root, destination = map(Path, sys.argv[1:])
with destination.open("w") as output:
    for path in sorted(root.rglob("*")):
        if path.is_file():
            output.write(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root)}\n")
PYHASH
cp "$source_dir/go.mod" "$work/go.mod.pin"
cp "$source_dir/go.sum" "$work/go.sum.pin"
export GOROOT="$go_prefix" GOTOOLCHAIN=local GOENV=off GOWORK=off
export GOOS="$target" GOARCH=amd64 GOAMD64=v1 CGO_ENABLED=0 GO111MODULE=on
export GOPROXY=https://repo.internal/repository/go-proxy/ GOSUMDB=off
export GONOPROXY=none GONOSUMDB=none GOPRIVATE= GOFLAGS=
export GOMODCACHE="$out/modcache" GOCACHE="$out/gocache"

"$go" -C "$source_dir" mod download
"$go" -C "$source_dir" mod verify
"$go" -C "$source_dir" mod vendor
cmp "$work/go.mod.pin" "$source_dir/go.mod"
cmp "$work/go.sum.pin" "$source_dir/go.sum"
# Apply the same ordered dependency revisions on both hosts.
for patch in "$source_dir"/patches/*.patch; do
  [[ -f "$patch" ]] || continue
  git -C "$source_dir" apply --whitespace=error-all -- "$patch"
done
export GOPROXY=off
"$go" -C "$source_dir" build -mod=vendor -p=8 -trimpath -o "$work/git" .

if [[ "$target" == pyxis ]]; then
  final_stage="$out/stage"
  stage="$work/stage"
  bundle="$stage/bin/git.pxb"
  executable="$bundle/app/bin/git.pxe"
  metadata="$bundle/app/metadata"
else
  final_stage="$out/stage-linux"
  stage="$work/stage-linux"
  executable="$stage/bin/git"
  metadata="$stage/share/git"
fi
# Only replace a previous build after the complete executable and notices exist.
if [[ "$target" == pyxis ]]; then
  install -D -m 644 "$source_dir/manifest.json" "$bundle/manifest.json"
fi
install -D -m 755 "$work/git" "$executable"
mkdir -p "$metadata/go" "$metadata/userland" "$metadata/git/patches"
install -m 644 "$go_prefix/LICENSE" "$metadata/go/LICENSE"
install -m 644 "$go_prefix/PATENTS" "$metadata/go/PATENTS"
install -m 644 "$go_prefix/share/pyxis-go/revision" "$metadata/go/revision"
install -m 644 "$repo/LICENSE" "$metadata/userland/LICENSE"
install -m 644 "$source_dir/go.mod" "$source_dir/go.sum" "$metadata/git/"
for patch in "$source_dir"/patches/*.patch; do
  [[ -f "$patch" ]] || continue
  install -m 644 "$patch" "$metadata/git/patches/"
done
python3 "$source_dir/stage-notices.py" "$source_dir" "$metadata" "$go_prefix"
install -m 644 "$work/source-sha256.txt" "$metadata/git/source-sha256.txt"
{
  echo "source_repository=$source_repository"
  echo "source_commit=$source_commit"
  echo "compiler=$("$go" version)"
  echo "target=$target/amd64; GOAMD64=v1; CGO_ENABLED=0"
  echo "module_proxy=https://repo.internal/repository/go-proxy/"
  echo "build=-mod=vendor -p=8 -trimpath"
  echo "profile=HTTPS clone/log/status; core.symlinks=false; core.fileMode=false"
  echo "main_license=MPL-2.0; dependencies retain upstream terms"
  echo "patches=git/patches; source-sha256.txt records exact copied source inputs"
} > "$metadata/git/SOURCE"
if [[ -e "$final_stage" ]]; then
  mv -- "$final_stage" "$work/previous-stage"
fi
relative_executable=${executable#"$stage/"}
mv -- "$stage" "$final_stage"
echo "$final_stage/$relative_executable"
