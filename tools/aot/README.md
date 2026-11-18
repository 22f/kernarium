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

`UML_AOT_MEMORY_CALL` (`--memory-call`) selects how a plain load or store
reaches its TLB probe: `inline` (the product), `probe` (a call to a routine of
the same unit that returns the compare; the miss exit stays at the site) or
`probe-miss` (the routine also takes the miss exit; the site names its
entry-table slot in a marker word). The call saves the guest's x30 in the
frame's last slot. The two calls are measurement variants of the same
contract; the differential and the guest runs hold each of them.

`UML_AOT_SHARED_EXITS` (`--shared-exits`) forms the high 16-bit chunks of an
exit target once per exit routine and chunk of a library, where the product
puts a `movk` in each exit stub; a stub then costs two words and the exit one
more branch. `UML_AOT_RUN_LIMIT` (`--run-limit`, 32 in the product) is the
longest run of instructions between charge points: a larger value inserts fewer
charge points and makes an indirect branch pay more, and the differential test's
bound on an entry follows it. Both are measurement variants with the same
contract. `UML_AOT_ENTRY_TABLE16` (`--entry-table16`) holds each library's
entry table in 16-bit words, with one signed 32-bit anchor per text page after
them, in place of 32 bits per source word: an indirect branch and a refill
then spend four more words to reach the native code, and the rewriter refuses a
page whose entries lie further apart than 256 KiB of code. The blob's magic
changes (`UMLAOT16`), so a runtime built without the option refuses it.

`UML_AOT_INSTANCE_CAPACITY` is the number of library instances each executor
thread's binder holds (64 in the product; above 64 it finds an instance by hash
instead of a scan). It is a runtime option: the blob does not change.

`UML_AOT_HOT_LIST` (`--hot-list`, with a call) keeps the inline probe in the
functions a profile calls hot and gives every other function the call.
`hot_list.py` makes the list from the guest sampler's profile files
(`darwin/cpu/arm64/gadgets/guest_sampler.hpp`): it matches each sampled block
by its instruction words against the executable segment of every covered
library, shares a block found in several places among them, and takes the
functions in order of samples (ties by library digest, then address) until
they hold `--coverage` of the samples. The list names each function by library
SHA-256 and range, and its header records the digest of the profile it came
from; the blob manifest records the list's digest (`hot_list_sha256`) and each
library's `hot_functions`. A library the list does not name is all cold. The
list is a build input, so a given profile and image make one blob.

Entry points: `ctest -R '^arm64-aot-tools$'` for the encoders, decoder and
assembler, and `ctest -R '^arm64-aot-differential$'` (a configuration with
`UML_AOT_DIAGNOSTICS=ON`) for the rewritten libraries against the gadget
engine.

Licence: the tool is GPL-2.0-or-later. The libraries it rewrites keep their
own licences. The framework is generated at build time from an accepted
image and staged into a local macOS application build for tests; nothing
built from it is published or released from this branch.
