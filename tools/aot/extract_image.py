#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack one read-only partition of an accepted Android release.

The release's system.img is a GPT disk whose partitions are Zstandard-
compressed EROFS images. The partition is copied out at the offset the
release's partition-layout.json gives and unpacked with an EROFS tool that
can decode Zstandard (the Android build's fsck.erofs; the Homebrew build
cannot). Reads the release only; writes only under --out.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=pathlib.Path, required=True,
                        help="an accepted release directory")
    parser.add_argument("--partition", default="system")
    parser.add_argument("--fsck-erofs", type=pathlib.Path, required=True,
                        help="an fsck.erofs that decodes Zstandard")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    arguments = parser.parse_args(argv)
    guest = arguments.release / "guest"
    layout = json.loads((guest / "partition-layout.json").read_text())
    matches = [p for p in layout["system_disk"]["partitions"]
               if p["name"] == arguments.partition]
    if len(matches) != 1:
        print(f"no partition named {arguments.partition}", file=sys.stderr)
        return 1
    partition = matches[0]
    manifest = json.loads((arguments.release / "release-manifest.json"
                           ).read_text())
    identity = {
        "release_id": manifest["release_id"],
        "partition": arguments.partition,
        "partition_sha256": partition["sha256"],
    }
    root = arguments.out / "root"
    stamp = arguments.out / "identity.json"
    if stamp.exists() and json.loads(stamp.read_text()) == identity and \
            root.is_dir():
        os.utime(stamp)  # the build system sees the output as current
        return 0
    if arguments.out.exists():
        shutil.rmtree(arguments.out)
    arguments.out.mkdir(parents=True)
    image = arguments.out / "partition.erofs"
    with open(guest / "system.img", "rb") as source, open(image, "wb") as sink:
        source.seek(partition["start_bytes"])
        remaining = partition["image_bytes"]
        while remaining:
            chunk = source.read(min(remaining, 8 << 20))
            if not chunk:
                print("short read of system.img", file=sys.stderr)
                return 1
            sink.write(chunk)
            remaining -= len(chunk)
    root.mkdir()
    subprocess.run([str(arguments.fsck_erofs), f"--extract={root}",
                    "--overwrite", str(image)], check=True,
                   stdout=subprocess.DEVNULL)
    image.unlink()
    stamp.write_text(json.dumps(identity, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
