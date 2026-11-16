# AOT rewriter

Build tool for the ahead-of-time component (`darwin/cpu/arm64/aot`). It
unpacks the partitions of an accepted Android release and rewrites the arm64 libraries named by
`UML_AOT_LIBRARIES` into one position-independent blob (`rewrite.py`), which
`darwin/cpu/arm64/aot/CMakeLists.txt` wraps in the signed
`UmlArm64Aot.framework`. The contract is in
[`docs/architecture.md`](../../docs/architecture.md#ahead-of-time-code); the
framework is generated and never committed.

- `elf64.py` reads program headers and function ranges (`.eh_frame` and
  `.plt`).
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
- `fixture.py` writes the small library of the differential test (a
  diagnostics build only): page-edge branches, loops that update one cell,
  and one function per instruction form.

Tools: the build needs a Python 3 and an `fsck.erofs` that decodes
Zstandard (`UML_AOT_FSCK_EROFS`), as the Android build produces; the
Homebrew `erofs-utils` is built without libzstd and cannot read the release
(CMake fails the configure step when the tool's `--help` does not list
`zstd`).

`UML_AOT_MEMORY_CALL` (`--memory-call`) selects how a plain load or store
reaches its TLB probe: `inline` (the product), `probe` (a call to a routine of
the same unit that returns the compare; the miss exit stays at the site) or
`probe-miss` (the routine also takes the miss exit; the site names its
entry-table slot in a marker word). The call saves the guest's x30 in the
frame's last slot. The two calls are measurement variants of the same
contract; the differential and the guest runs hold each of them.

Entry points: `ctest -R '^arm64-aot-tools$'` for the encoders, decoder and
assembler, and `ctest -R '^arm64-aot-differential$'` (a configuration with
`UML_AOT_DIAGNOSTICS=ON`) for the rewritten libraries against the gadget
engine.

Licence: the tool is GPL-2.0-or-later. The libraries it rewrites keep their
own licences. The framework is generated at build time from an accepted
image and staged into a local macOS application build for tests; nothing
built from it is published or released from this branch.
