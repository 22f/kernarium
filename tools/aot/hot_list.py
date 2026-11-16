#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Which functions of the rewritten libraries a profile says are hot.

Input: the guest sampler's profile files (darwin/cpu/arm64/gadgets/
guest_sampler.hpp) from runs of the engine alone, and the libraries the AOT
blob covers. The sampler names a block by its first instruction words, not by
an address that means anything outside one run, so every block is searched for
in the executable segment of every library (all aligned positions); a block
found in several places shares its samples among them.

Output: the smallest set of functions, taken in order of samples, that holds
`--coverage` of the samples attributed to the libraries. The rewriter inlines
the memory probe of these functions and gives the rest the cold shape of
`--memory-call` (tools/aot/rewrite.py --hot-list). The file lists, for each
function, the SHA-256 of the library file, the function's range and its
samples, so it is deterministic for a profile and a library set, and the
header records the digest of the profile it was made from.

Selection is by samples only: the order is samples descending, then library
digest, then address, so equal weights always fall the same way.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import array
import bisect
import collections
import hashlib
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import elf64  # noqa: E402
import rewrite  # noqa: E402


def read_profile(paths: list[pathlib.Path]) -> tuple[collections.Counter, str]:
    """(first words of a block) -> samples, over all profile files, and the
    digest of the files' contents in the order given."""
    blocks: collections.Counter = collections.Counter()
    digest = hashlib.sha256()
    for path in paths:
        data = path.read_bytes()
        digest.update(hashlib.sha256(data).digest())
        for line in data.decode().splitlines():
            fields = line.split("\t")
            if len(fields) < 5 or fields[0] != "guest":
                continue
            words = tuple(int(word, 16) for word in fields[4].split(","))
            if len(words) >= 2:
                blocks[words] += int(fields[1])
    return blocks, digest.hexdigest()


def functions_of(elf: elf64.Elf, segment) -> list[tuple[int, int]]:
    """The functions the rewriter covers (rewrite.Library.functions)."""
    return sorted((function.start, function.end)
                  for function in elf.functions()
                  if segment.vaddr <= function.start and
                  function.end <= segment.vaddr + segment.filesz and
                  function.start % 4 == 0 and function.end % 4 == 0)


def attribute(blocks: collections.Counter, libraries: list[dict]) -> None:
    """Add to each library's `weight` (function start -> samples) the share of
    every block that is found in its text. A block found n times in all
    places adds 1/n of its samples at each."""
    wanted: dict[int, list[tuple[int, ...]]] = collections.defaultdict(list)
    for words in blocks:
        wanted[words[0] | (words[1] << 32)].append(words)
    hits: dict[tuple[int, ...], list[tuple[dict, int]]] = \
        collections.defaultdict(list)
    for library in libraries:
        text = library["text"]
        base = library["vaddr"]
        for shift in (0, 4):
            body = text[shift:]
            body = body[:len(body) // 8 * 8]
            values = array.array("Q")
            values.frombytes(body)
            for index, value in enumerate(values):
                candidates = wanted.get(value)
                if not candidates:
                    continue
                at = shift + 8 * index
                for words in candidates:
                    end = at + 4 * len(words)
                    if end <= len(text) and struct.unpack_from(
                            f"<{len(words)}I", text, at) == words:
                        hits[words].append((library, base + at))
    for words, places in hits.items():
        share = blocks[words] / len(places)
        for library, offset in places:
            starts = library["starts"]
            index = bisect.bisect_right(starts, offset) - 1
            if index < 0:
                continue
            start, end = library["ranges"][index]
            if start <= offset < end:
                library["weight"][start] += share


def load_libraries(root: pathlib.Path, mounts: dict[str, pathlib.Path],
                   guests: list[str]) -> list[dict]:
    """The libraries to attribute samples to, one per distinct file: a file
    listed under two guest paths is one library to the rewriter's digest."""
    libraries: list[dict] = []
    seen: set[str] = set()
    for guest in guests:
        path = rewrite.resolve_library(root, mounts, guest)
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        elf = elf64.Elf(data)
        segment = elf.executable_segment()
        ranges = functions_of(elf, segment)
        libraries.append({
            "guest": guest,
            "sha256": digest,
            "vaddr": segment.vaddr,
            "text": elf.read_vaddr(segment.vaddr, segment.filesz),
            "ranges": ranges,
            "starts": [start for start, _ in ranges],
            "weight": collections.Counter(),
        })
    return libraries


def choose(rows: list[tuple], coverage: float
           ) -> tuple[list[tuple], float, float]:
    """The rows (samples, library digest, start, end, path) in order of
    samples that first hold `coverage` of all the samples, by library digest
    and address; the samples held and the total."""
    rows = sorted(rows, key=lambda row: (-row[0], row[1], row[2]))
    total = sum(row[0] for row in rows)
    goal = coverage * total
    chosen = []
    held = 0.0
    for row in rows:
        if held >= goal:
            break
        chosen.append(row)
        held += row[0]
    chosen.sort(key=lambda row: (row[1], row[2]))
    return chosen, held, total


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=pathlib.Path, required=True,
                        help="the unpacked system partition (as rewrite.py)")
    parser.add_argument("--mount", action="append", default=[],
                        metavar="GUESTPREFIX=DIR")
    parser.add_argument("--library", action="append", default=[],
                        help="a library's guest path; repeatable")
    parser.add_argument("--coverage", type=float, required=True,
                        help="the share of the attributed samples the hot "
                        "functions hold (0 to 1)")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("profiles", nargs="+", type=pathlib.Path)
    arguments = parser.parse_args(argv)
    if not 0.0 < arguments.coverage <= 1.0:
        parser.error("--coverage must be in (0, 1]")
    mounts: dict[str, pathlib.Path] = {}
    for item in arguments.mount:
        guest, _, directory = item.partition("=")
        mounts[guest.rstrip("/")] = pathlib.Path(directory)
    blocks, profile_digest = read_profile(arguments.profiles)
    libraries = load_libraries(arguments.root, mounts, arguments.library)
    attribute(blocks, libraries)
    rows = []
    for library in libraries:
        ends = dict(library["ranges"])
        for start, weight in library["weight"].items():
            rows.append((weight, library["sha256"], start, ends[start],
                         library["guest"]))
    chosen, held, total = choose(rows, arguments.coverage)
    with open(arguments.out, "w") as handle:
        handle.write(f"# {rewrite.HOT_LIST_FORMAT}\n")
        handle.write(f"# profile-sha256 {profile_digest}\n")
        handle.write(f"# coverage {arguments.coverage}\n")
        handle.write(f"# samples {total:.1f} held {held:.1f} "
                     f"functions {len(chosen)} of {len(rows)} sampled\n")
        for weight, sha, start, end, guest in chosen:
            handle.write(f"{sha}\t{start:#x}\t{end:#x}\t{weight:.2f}\t{guest}\n")
    print(f"{len(chosen)} functions of {len(rows)} sampled hold "
          f"{held / total * 100 if total else 0:.1f}% of {total:.0f} samples "
          f"in {len(libraries)} libraries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
