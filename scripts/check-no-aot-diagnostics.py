#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail if a packaged product carries the AOT runtime's diagnostics build.

UML_AOT_DIAGNOSTICS composes a second compilation of the AOT runtime with
counters, a report writer and the test switches. The unit row
`arm64-aot-runtime-product` checks the product's own object; this check reads
what a package or a built product linked. Every Mach-O binary and static
archive below each path must hold neither the diagnostics namespace nor the
report environment name, which outlive a stripped symbol table.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path


# Mach-O magics of both byte orders, thin and universal, and the ar magic.
MAGICS = (
    bytes.fromhex("feedfacf"),
    bytes.fromhex("cffaedfe"),
    bytes.fromhex("feedface"),
    bytes.fromhex("cefaedfe"),
    bytes.fromhex("cafebabe"),
    bytes.fromhex("bebafeca"),
    b"!<arch>\n",
)
# The AOT diagnostics namespace in a mangled name (the gadget engine has a
# namespace of the same last name, so the match includes `aot`), and the
# variable only the diagnostics compilation reads.
MARKERS = (b"3aot11diagnostics", b"DARWIN_UML_AOT_REPORT")


def binaries(root: Path) -> list[Path]:
    candidates = [root] if root.is_file() else sorted(root.rglob("*"))
    found = []
    for path in candidates:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as handle:
            head = handle.read(8)
        if any(head.startswith(magic) for magic in MAGICS):
            found.append(path)
    return found


def problems(path: Path) -> list[str]:
    data = path.read_bytes()
    return [
        f"{path}: holds the AOT diagnostics build's {marker.decode()}"
        for marker in MARKERS
        if marker in data
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    arguments = parser.parse_args()

    checked = 0
    failures = []
    for root in arguments.paths:
        if not root.exists():
            failures.append(f"{root}: does not exist")
            continue
        found = binaries(root)
        if not found:
            failures.append(f"{root}: holds no Mach-O binary or archive")
        for path in found:
            checked += 1
            failures.extend(problems(path))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"no AOT diagnostics build in {checked} binaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
