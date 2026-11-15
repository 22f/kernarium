# AOT rewriter

Build tool for the ahead-of-time component (`darwin/cpu/arm64/aot`). It
unpacks the system partition of an accepted Android release
(`extract_image.py`) and rewrites the arm64 libraries named by
`UML_AOT_LIBRARIES` into one position-independent blob (`rewrite.py`), which
`darwin/cpu/arm64/aot/CMakeLists.txt` wraps in the signed
`UmlArm64Aot.framework`. The contract is in
[`docs/architecture.md`](../../docs/architecture.md#ahead-of-time-code); the
framework is generated and never committed.

- `elf64.py` reads program headers and function ranges (`.eh_frame`).
- `a64decode.py` identifies an instruction exactly from
  `tests/unit/arm64/a64_instruction_decode_partitions.tsv`.
- `asm.py` encodes the words the rewriter emits and resolves labels.
- `rewrite.py` classifies each word of each function (inline or step), emits
  the code, the per-instruction entry table, the page digests and the digest
  hash. Every instruction it does not inline is left to the engine, so it
  never refuses for correctness.

Entry points: `ctest -R '^arm64-aot-tools$'` for the encoders and
`ctest -R '^arm64-aot-libz$'` for the rewritten libz against the gadget
engine.

Licence: the tool is GPL-2.0-or-later. The libraries it rewrites keep their
own licences; the framework built from them is not redistributed.
