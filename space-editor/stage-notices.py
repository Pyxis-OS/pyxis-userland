#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Retain complete upstream notices for the modules compiled into spaces.

Adapted from userland git/stage-notices.py (MPL-2.0).
"""
import json
import os
from pathlib import Path
import subprocess
import sys

source, metadata, go = map(Path, sys.argv[1:4])
env = dict(os.environ, GOROOT=str(go), GOTOOLCHAIN="local", GOPROXY="off",
           GOSUMDB="off", GOENV="off", GOWORK="off", GOFLAGS="")
command = [str(go / "bin/go"), "-C", str(source)]
lines = subprocess.check_output(
    command + ["list", "-mod=vendor", "-deps", "-f",
               "{{if .Module}}{{.Module.Path}} {{.Module.Version}}{{end}}", "."],
    env=env, text=True).splitlines()
modules = set()
for line in lines:
    fields = line.split()
    if len(fields) == 2:
        modules.add(tuple(fields))
    elif fields and fields != ["pyxis.local/spaces"]:
        raise RuntimeError(f"unversioned compiled module: {line}")
sums = {}
for line in (source / "go.sum").read_text().splitlines():
    name, version, checksum = line.split()
    if not version.endswith("/go.mod"):
        sums[name, version] = checksum
(metadata / "dependencies").mkdir(parents=True)
with (metadata / "spaces/modules.txt").open("w") as output:
    output.write("# Compiled modules; original go.sum pins, before recorded patches.\n")
    output.write("pyxis.local/spaces main source-sha256.txt\n")
    for name, version in sorted(modules):
        output.write(f"{name} {version} {sums[name, version]}\n")
with (metadata / "dependencies/NOTICES.txt").open("wb") as output:
    for name, version in sorted(modules):
        # vendor omits some upstream files. Read the complete original cached
        # module, already authenticated/downloaded before the offline build.
        info = json.loads(subprocess.check_output(
            command + ["mod", "download", "-json", f"{name}@{version}"],
            env=env, text=True))
        if info.get("Error"):
            raise RuntimeError(info["Error"])
        root = Path(info["Dir"])
        notices = [path for path in root.rglob("*") if path.is_file() and
                   path.name.lower().startswith(
                       ("license", "copying", "notice", "copyright", "authors", "patents"))]
        if not notices:
            raise RuntimeError(f"no upstream notices: {name} {version}")
        for path in sorted(notices):
            output.write(f"\n===== {name} {version}: {path.relative_to(root)} =====\n\n".encode())
            output.write(path.read_bytes())
            output.write(b"\n")
