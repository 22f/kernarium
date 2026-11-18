# AOT rewriter

Build tool for the ahead-of-time component (`darwin/cpu/arm64/aot`). It
unpacks the partitions of an accepted Android release and rewrites the arm64 libraries named by
`UML_AOT_LIBRARIES` and the release's boot image (`UML_AOT_BOOT_CODE`) into
one position-independent blob (`rewrite.py`), which
`darwin/cpu/arm64/aot/CMakeLists.txt` wraps in the signed
`UmlArm64Aot.framework`. The contract is in
[`docs/architecture.md`](../../docs/architecture.md#ahead-of-time-code); the
framework is generated and never committed.

- `elf64.py` reads program headers and function ranges (`.eh_frame` and
  `.plt`; for an OAT file, which has no unwind table, the function symbols of
  its `.gnu_debugdata`).
- `a64decode.py` identifies an instruction exactly from
  `tests/unit/arm64/a64_instruction_decode_partitions.tsv`.
- `asm.py` encodes the words the rewriter emits and resolves labels.
- `rewrite.py` classifies each word of each function (inline or step), emits
  the code, the per-instruction entry table, the SHA-256 of each text page
  (what binds the code to the guest bytes), the page digests and the digest
  hash. Every instruction it does not inline is left to the engine, so it
  never refuses for correctness.

- `extract_image.py` unpacks the partitions and APEX payloads the libraries
  come from; `licences.py` records, per library, the notice the image itself
  states (generated beside the framework, not tracked).
- `fixture.py` writes the small libraries of the differential test (a
  diagnostics build only): page-edge branches, loops that update one cell,
  and one function per instruction form; and a file shaped like an OAT file
  (no unwind table, symbols in `.gnu_debugdata`, methods that touch, direct
  branches that reach below the library).

Boot code: every `boot*.oat` of the release that has an executable segment is
translated whole, with no profile (an OAT file with no compiled method has no
code and is listed in the manifest's `skipped`). A `--library` path may end in
a pattern, which names every file it matches in name order. Every component of
a `--library` path, a directory before the last one as well, is resolved inside
the image as the guest would resolve it (an absolute link from the image's root,
`..` stopping there); a link never reaches a host file outside the unpacked
image. A named file that is not a regular file of the image is refused, and a
match that is not is listed in `skipped`. The OAT files of
one boot image are mapped next to each other and call each other by direct
branches, so a branch from a later file into an earlier one reaches below the
library's own first byte: its target is formed as a full address and leaves by
the absolute branch exit.

One path names one distinct file. A unit binds by the content of its pages,
so it serves every identical copy that another APEX or partition ships; the
build refuses a path listed twice, and two paths whose files are the same
bytes.

Tools: the build needs a Python 3 and an `fsck.erofs` that decodes
Zstandard (`UML_AOT_FSCK_EROFS`), as the Android build produces; the
Homebrew `erofs-utils` is built without libzstd and cannot read the release
(CMake fails the configure step when the tool's `--help` does not list
`zstd`).

The blob's magic names its layout and the word after it the layout's version.
A change a runtime built for the old layout would misread changes the magic; an
addition it can ignore raises the version (`BLOB_VERSION`), and a runtime
accepts any version at or above the one it was built for.

The entry table holds 16 bits per source word (the offset of the word's native
code, in words, from its page's anchor) and one signed 32-bit anchor per text
page, half the bytes of a 32-bit entry per word; the blob's magic is
`UMLAOT16`. An indirect branch and a refill spend four words more to reach the
native code than a table of 32-bit entries would. A page whose entries lie
further than 256 KiB of code apart is a row of branches when it holds the end
of a function of 32 KiB or more. A library whose table cannot be formed (a page
still wider, or more than 32 MiB of text) is not in the blob: the engine runs
it, the manifest's `refused` lists it, and the diagnostics report counts it
(`libraries_refused`, from the header; `BLOB_VERSION` 2).

`UML_AOT_INSTANCE_CAPACITY` is the number of library instances each executor
thread's binder holds (64 in the product; above 64 it finds an instance by hash
instead of a scan). It is a runtime option: the blob does not change.

Entry points: `ctest -R '^arm64-aot-tools$'` for the encoders, decoder and
assembler, and `ctest -R '^arm64-aot-differential$'` (a configuration with
`UML_AOT_DIAGNOSTICS=ON`) for the rewritten libraries against the gadget
engine.

Licence: the tool is GPL-2.0-or-later. The libraries it rewrites keep their
own licences. The framework is generated at build time from an accepted
image and staged into a local macOS application build for tests; nothing
built from it is published or released from this branch.
