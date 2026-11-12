#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a built Andarium application against the AppStore configuration.

The App Store configuration (Andarium/README.md#configurations) builds an
application that must:

  - declare no non-exempt encryption, require arm64 and Metal, target iPhone
    and iPad, and have no background mode;
  - carry no automation knob: no ANDARIUM_, KERNARIUM_, UML_ or DARWIN_UML_
    environment name (ANDARIUM_THERMAL_STATE, which states the thermal state
    in place of iOS's, among them) in the host code of the application or of
    any embedded framework (the guest kernel image inside a runtime is not
    host code);
  - carry none of the harness's console lines (ConsoleReport in the
    application: the statistics, thermal state, runtime and focus lines),
    which the configuration compiles out;
  - carry no debugging or automation support in the host code of the
    application or of a framework: the frame mirror, console input, the
    guest sampler, the footprint log, the virtual Jetsam, the activity logs,
    the census and histogram, the automation entries, the synthetic camera, the
    simulated controllers, the UI tests' input probe and the harness's
    leftovers (DEBUG_SYMBOLS and DEBUG_STRINGS name them; the stand-ins that
    keep the runtime API's layout, runtimeSetFrameSink, runtimeSendConsoleInput
    and runtimePrepareJit, are allowed), and contain no test bundle and no
    XCTest or Testing framework;
  - carry no JIT: no arm64 JIT engine, no executable-memory owner, none of the
    runtime's or the application's JIT entries or text, and no instruction
    word of the debugger request (`brk #0xf00d`, JIT_DEBUGGER_REQUEST) in the
    code of the application or of a framework (the JIT is for the other
    configurations, which a debugger prepares; an App Store build has no
    executable memory of its own);
  - carry no JIT text in a language: no JIT strings table in any lproj, and no
    localized key or value that names JIT or StikDebug (the JIT texts are a
    String Catalog of their own that the configuration excludes);
  - carry no trace of the machine that built it: no path under a user's home
    directory (/Users/<name>/), on a mounted volume (/Volumes/<name>/) or in
    the per-user temporary directories (/var/folders/) in the host code of an
    executable, and no debug stabs in an embedded framework (the linker's
    debug map, whose N_OSO entries name the object files of the build host's
    checkout and dependency trees; dsymutil has read it into the dSYM by then,
    and strip -S removes it and keeps the UUID). The strings of those stabs
    are not searched for a path. Xcode strips the application's own
    executable when it archives, and strips nothing it embeds. A path that a
    dependency compiles in from its prefix is the dependencies' to avoid
    (dependencies/README.md, "No build-host paths"); and
  - pass scripts/check-privacy-manifests.py; and
  - carry Send feedback (Andarium/README.md#send-feedback) only when the build
    names a receiver: the feature is compiled in exactly where the ignored
    local configuration holds an address, in this configuration alone. A
    product without an address holds nothing of it: no type, text, framework
    or Info.plist key; a product with one holds all of it and the address
    the build named. The local configuration's receiver is in no file of a
    bundle whose build does not name it, which the check finds by its bytes
    and never prints. Info.plist keeps every value of Config/Info.plist, as
    its preprocessing is to change only the feedback key. The feature's
    texts are the String Catalog table Feedback, which the build compiles in
    with the feature and no other: a product without it has no Feedback
    strings table, found by the file name whatever it holds, and a product
    with it has the table in Japanese and both Chinese; and
  - carry Follow developer on X (Andarium/README.md#follow-developer-on-x)
    only when the build names a handle, by the same rule: compiled in exactly
    where the ignored local configuration holds a handle and the ignored
    Local directory holds the logo, in this configuration alone. A product
    without a handle holds nothing of it: no type, Info.plist key, FollowX
    strings table or logo in its asset catalog; a product with one holds all
    of it, the handle the build named, and a logo. The local handle is in no
    file of a bundle whose build does not name it, which the check finds as
    a profile address or as a property list's whole string (a bare scan of
    its bytes would match the bundle identifier, which holds the same word),
    and never prints.

The knob scan reads bytes, so it does not see a Swift string of up to 15 bytes
(ANDARIUM_START is one): the compiler keeps those in instruction immediates.
The launch names stay out of the application because the configuration
compiles Live/LaunchEnvironment.swift out, and the scan covers the longer names.
The console lines are long enough to be text in the binary, so a build whose
code still holds one fails, whether or not anything calls it.

The Xcode project runs it as the last build phase of the App Store
configuration, so a build that breaks one of these rules fails. In every other
configuration a phase runs `--feedback --follow-x`, which checks the two optional
features alone: there each is absent unless a test or capture script arranged it
with its example value, in Debug alone. Both print one line per feature that
says whether it is compiled in.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

KNOB = re.compile(rb"\b(?:ANDARIUM|KERNARIUM|DARWIN_UML|UML)_[A-Z][A-Z0-9_]{2,}\b")
# What ConsoleReport (Shared/ConsoleReport.swift) prints, as far as its text is
# the same in every line; the configuration compiles the type out.
HARNESS_LINES = re.compile(
    rb"Andarium stats|Andarium thermal state|Andarium: runtime returned|Andarium: application ")
# What a path names about the machine that built the code: the account whose
# home directory held the checkout, the volume that held the checkout or the
# dependency prefixes, and the build's temporary directories. A bare
# "/Volumes/" or a format such as "/Volumes/%s" is no path of a build and
# does not match.
HOST_PATHS = (
    ("a path under a home directory",
     re.compile(rb"/Users/[A-Za-z0-9._-]+/[\x21-\x7e]*")),
    ("a path on a mounted volume",
     re.compile(rb"/Volumes/[A-Za-z0-9._ -]+/[\x21-\x7e]*")),
    ("a path in a temporary build directory",
     re.compile(rb"/(?:private/)?var/folders/[\x21-\x7e]+")),
)
# Sections that hold the guest, not the host: the Linux kernel image a runtime
# carries, and the boot files of a Linux runtime.
GUEST_SECTIONS = {("__TEXT", "__uml_elf"), ("__TEXT", "__uml_boot_base"), ("__TEXT", "__uml_boot_init")}
# Where the host's own instructions are. A word that is the debugger request
# (below) in a section of data is no request.
CODE_SECTIONS = {("__TEXT", "__text")}
# `brk #0xf00d`: the instruction a process uses to ask the debugger it is
# attached to for JIT memory (darwin/cpu/jit_memory/executable_memory.cpp).
# Only the debugger-route build of the runtime contains it.
JIT_DEBUGGER_REQUEST = bytes.fromhex("a0013ed4")

MACHO_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_STAB = 0xE0
N_SO = 0x64
N_OSO = 0x66
N_SOL = 0x84
# The stabs whose strings are paths: the compilation directory and source
# file, the object file, and an included file. The others name symbols.
PATH_STABS = frozenset({N_SO, N_OSO, N_SOL})
CHECKER = Path(__file__).resolve().parents[2] / "scripts" / "check-privacy-manifests.py"

# Send feedback. The build writes the receiver to Info.plist under this key
# only where the feature is compiled in (Andarium/Config/Andarium.xcconfig).
FEEDBACK_KEY = "AndariumFeedbackAddress"
# The only address a configuration other than AppStore may carry: the one the
# test and capture scripts arrange.
FEEDBACK_ARRANGED = "feedback@example.com"
# What the feature leaves in a binary: its types (Swift writes a name's length
# before it, so a fragment of the name matches), the subject's prefix, the
# Info.plist key it reads, and the system frameworks only it uses.
FEEDBACK_REQUIRED = (b"Andarium feedback: ", FEEDBACK_KEY.encode(), b"MessageUI.framework")
FEEDBACK_FRAGMENTS = (
    b"SendFeedback", b"MFMailCompose", b"PhotosUI.framework", *FEEDBACK_REQUIRED)
# Words the feature puts in a String Catalog.
FEEDBACK_WORDS = ("Send feedback", "Andarium feedback")
# The feature's own table (Localization/Feedback.xcstrings), which a String
# Catalog compiles to Feedback.strings, and to Feedback.stringsdict for a plural,
# in each language's directory; and the languages that table has besides English.
FEEDBACK_TABLE = "Feedback"
FEEDBACK_LANGUAGES = ("ja", "zh-Hans", "zh-Hant")
# Follow developer on X. The build writes the developer's handle to Info.plist
# under this key only where the row is compiled in (Andarium/Config/Andarium.xcconfig).
FOLLOW_X_KEY = "AndariumFollowXHandle"
# The only handle a configuration other than AppStore may carry: the one the test
# and capture scripts arrange, and the placeholder of Developer.local.xcconfig.example.
FOLLOW_X_ARRANGED = "example_handle"
# What the row leaves in a binary: its types (Swift writes a name's length before
# it, so a fragment of the name matches), the line of its text that is too long
# to sit in an instruction, and the Info.plist key it reads.
FOLLOW_X_REQUIRED = (b"Follow developer on", FOLLOW_X_KEY.encode())
FOLLOW_X_FRAGMENTS = (b"FollowX", *FOLLOW_X_REQUIRED)
# Words the row puts in a String Catalog.
FOLLOW_X_WORDS = ("Follow developer on",)
# The row's own table (Localization/FollowX.xcstrings), by the same rule as Feedback.
FOLLOW_X_TABLE = "FollowX"
# The logo's name in the asset catalog the build compiles in with the row
# (Assets.car keeps an asset's name as plain text), and the file names of the
# artwork, which no bundle carries as a file.
FOLLOW_X_ASSET = b"XLogo"
FOLLOW_X_FILES = frozenset({"x-logo.svg", "glyph.svg"})
# The shape of a handle of X, the same rules as FollowXProfile.isWellFormed.
FOLLOW_X_HANDLE_PATTERN = re.compile(r"[A-Za-z0-9_]{1,15}")
# The hosts a profile address of the developer could name.
FOLLOW_X_HOSTS = ("x.com", "twitter.com")
# Domains and top-level domains that name no mailbox (RFC 2606, RFC 6761).
PLACEHOLDER_DOMAINS = ("example.com", "example.org", "example.net")
PLACEHOLDER_TLDS = ("example", "invalid", "localhost", "test")


def thin_offset(data: bytes) -> int | None:
    if len(data) < 8:
        return None
    if struct.unpack_from(">I", data, 0)[0] == FAT_MAGIC:
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(min(count, 16)):
            cpu, _sub, offset, _size, _align = struct.unpack_from(
                ">IIIII", data, 8 + 20 * index)
            if cpu == CPU_TYPE_ARM64:
                return offset
        return None
    return 0 if struct.unpack_from("<I", data, 0)[0] == MACHO_64 else None


def section_ranges(
    data: bytes, base: int, wanted: set[tuple[str, str]],
) -> list[tuple[int, int]]:
    """File ranges of the named (segment, section) pairs of one Mach-O slice."""
    found = []
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SEGMENT_64:
            sections = struct.unpack_from("<I", data, position + 64)[0]
            entry = position + 72
            for _ in range(sections):
                name = data[entry:entry + 16].rstrip(b"\0").decode()
                segment = data[entry + 16:entry + 32].rstrip(b"\0").decode()
                length, offset = struct.unpack_from("<QI", data, entry + 40)
                if (segment, name) in wanted:
                    found.append((base + offset, base + offset + length))
                entry += 80
        position += size
    return found


def guest_ranges(data: bytes, base: int) -> list[tuple[int, int]]:
    """File ranges of the sections that hold the guest."""
    return section_ranges(data, base, GUEST_SECTIONS)


def host_code(path: Path) -> tuple[bytes, int] | None:
    """The bytes of one Mach-O file with the guest's sections blanked, and the
    offset of its arm64 slice; None for a file that is not an arm64 Mach-O."""
    data = path.read_bytes()
    base = thin_offset(data[:4096])
    if base is None:
        return None
    buffer = bytearray(data)
    for start, end in guest_ranges(data, base):
        buffer[start:end] = bytes(end - start)
    return bytes(buffer), base


def knobs(path: Path) -> list[str]:
    """The automation knob names in the host code of one Mach-O file."""
    code = host_code(path)
    if code is None:
        return []
    return sorted({match.decode() for match in KNOB.findall(code[0])})


def harness_lines(path: Path) -> list[str]:
    """The harness's console lines in the host code of one Mach-O file."""
    code = host_code(path)
    if code is None:
        return []
    return sorted({match.decode().strip() for match in HARNESS_LINES.findall(code[0])})


def symbol_table(data: bytes, base: int) -> tuple[int, int, int, int] | None:
    """The symbol table of a Mach-O: the offset and count of its struct
    nlist_64 entries, and the offset and size of its string table."""
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SYMTAB:
            offset, symbols, strings, strings_size = struct.unpack_from(
                "<IIII", data, position + 8)
            return base + offset, symbols, base + strings, strings_size
        position += size
    return None


def debug_stabs(data: bytes, base: int) -> tuple[int, int]:
    """The debug stabs in a Mach-O's symbol table, and how many of them are
    N_OSO, each of which names an object file."""
    table = symbol_table(data, base)
    if table is None:
        return 0, 0
    offset, symbols, _strings, _size = table
    # struct nlist_64 is 16 bytes and n_type is its fifth.
    kinds = data[offset + 4:offset + 16 * symbols:16]
    return sum(1 for kind in kinds if kind & N_STAB), kinds.count(N_OSO)


def without_debug_map(data: bytes, base: int, kinds: frozenset[int] | None = None) -> bytes:
    """The Mach-O with the strings of its debug stabs blanked: the names of
    the objects, sources and functions the linker recorded. A framework that
    carries them fails for that, and Xcode strips them from the application's
    own executable when it archives, so neither is a path the build host left
    in the code. `kinds` limits the blanking to those stab types (PATH_STABS):
    the linker shares a string between a symbol and its stab, so blanking a
    function's stab would blank the symbol the code carries as well."""
    table = symbol_table(data, base)
    if table is None:
        return data
    offset, symbols, strings, size = table
    buffer = bytearray(data)
    for entry in range(symbols):
        string, kind = struct.unpack_from("<IB", data, offset + 16 * entry)
        if kind & N_STAB and (kinds is None or kind in kinds) and 0 < string < size:
            end = data.find(b"\0", strings + string, strings + size)
            if end > 0:
                buffer[strings + string:end] = bytes(end - strings - string)
    return bytes(buffer)


def build_host_traces(path: Path, debug_map: bool = True) -> list[str]:
    """What one Mach-O file keeps of the machine that built it. `debug_map`
    says whether its debug stabs count: they do for a framework, whose binary
    ships as linked. The strings of the stabs are never searched for a path:
    a framework with a debug map fails on the map, and the application's own
    executable loses it when Xcode archives."""
    code = host_code(path)
    if code is None:
        return []
    data, base = code
    found = []
    stabs, objects = debug_stabs(data, base) if debug_map else (0, 0)
    if stabs:
        found.append(
            f"carries the linker's debug map ({stabs} stabs, {objects} N_OSO "
            "object paths); strip -S it after dsymutil")
    data = without_debug_map(data, base)
    for label, pattern in HOST_PATHS:
        paths = sorted({match.decode() for match in pattern.findall(data)})
        if paths:
            found.append(
                f"names {label} ({len(paths)}): "
                f"{paths[0][:80]}{' ...' if len(paths) > 1 else ''}")
    return found


# The permissions Andarium asks of the person, one capability each: Camera,
# Microphone and Location.
USAGE_DESCRIPTIONS = (
    "NSCameraUsageDescription",
    "NSMicrophoneUsageDescription",
    "NSLocationWhenInUseUsageDescription",
)


def info_problems(info: dict) -> list[str]:
    found = []
    if info.get("ITSAppUsesNonExemptEncryption") is not False:
        found.append("Info.plist: ITSAppUsesNonExemptEncryption must be false")
    capabilities = info.get("UIRequiredDeviceCapabilities")
    for capability in ("arm64", "metal"):
        if not isinstance(capabilities, list) or capability not in capabilities:
            found.append(f"Info.plist: UIRequiredDeviceCapabilities lacks {capability}")
    if info.get("UIDeviceFamily") != [1, 2]:
        found.append("Info.plist: UIDeviceFamily must be [1, 2], iPhone and iPad")
    if "UIBackgroundModes" in info:
        found.append("Info.plist: the application has no background mode")
    # The system shows these when the person turns a capability on, and ends
    # the application at the request of a capability whose text is missing.
    for key in USAGE_DESCRIPTIONS:
        value = info.get(key)
        if not isinstance(value, str) or not value.strip():
            found.append(f"Info.plist: {key} must be a non-empty string")
    return found


def executables_of(bundle: Path) -> list[Path]:
    """The executable of one bundle, if its Info.plist names one."""
    try:
        with (bundle / "Info.plist").open("rb") as handle:
            name = plistlib.load(handle).get("CFBundleExecutable")
    except (OSError, plistlib.InvalidFileException, ValueError):
        return []
    return [bundle / name] if isinstance(name, str) and (bundle / name).is_file() else []


def executables(app: Path) -> list[Path]:
    """The executables of the application and of its embedded frameworks."""
    return [
        binary
        for bundle in [app, *sorted((app / "Frameworks").glob("*.framework"))]
        for binary in executables_of(bundle)
    ]


# The debugging and automation support an App Store runtime and application
# leave out (Andarium/README.md#configurations). Each entry is a kind and the
# fragments that name its machinery: mangled symbol names (C++ and Swift write
# a name's length before it) and the text its log lines hold. The stand-ins that
# keep the runtime API table's layout (runtimeSetFrameSink and
# runtimeSendConsoleInput return -ENOTSUP) are not on the list. The scan
# reads the host code, so a Swift name of up to 15 bytes in an instruction is
# not seen; the symbol fragments are what catch the application's Swift.
DEBUG_SYMBOLS = (
    ("frame mirror", (b"16FrameTapRegistry", b"11setFrameTap", b"14renderTapFrame",
                      b"7tapLoop", b"kernarium_runtime_loader_set_frame_sink")),
    ("console input", (b"13InputRegistry", b"7HostEnd4send",
                       b"kernarium_runtime_loader_send_console_input")),
    ("guest sampler", (b"11diagnostics12_GLOBAL__N_17Sampler",)),
    ("footprint log", (b"17FootprintReporter",)),
    ("virtual Jetsam", (b"13VirtualJetsam7observe", b"format_virtual_jetsam_line",
                        b"sample_process_footprint")),
    ("activity log", (b"18DisplayActivityLog", b"5video11ActivityLog")),
    ("census and histogram", (b"6census", b"execution_histogram_note",
                              b"execution_histogram_forget")),
    ("automation", (b"kernarium_runtime_automation_",
                    b"kernarium_runtime_loader_automation_", b"7harness",
                    b"14AutomationBoot")),
    ("test tooling", (b"5vsock9Forwarder", b"17UnixSocketChannel", b"_rfbGetScreen",
                      b"_rfbInitServer")),
    ("launch harness", (b"13ConsoleReport", b"15environmentName", b"13applyDeferred",
                        b"16printsStatistics", b"13launchApplied",
                        b"15ThermalOverride")),
    # The Simulator preview's denied-network notice (Model.previewNetworkNotice):
    # the Simulator cannot be denied the network, so the preview plays it.
    ("network notice preview", (b"20previewNetworkNotice",)),
    # The Simulator preview's hooks into the Send feedback sheet
    # (SendFeedbackServices.previewOpensAtLaunch and previewPrepare): the
    # preview opens the sheet and fills a draft; the application never does.
    ("send feedback preview", (b"20previewOpensAtLaunch", b"14previewPrepare")),
    # The UI tests' probe of the guest's keyboard and mouse
    # (Andarium/Shared/InputProbe.swift): its log, its launch, the recording
    # runtime and the session that runs without a guest.
    ("input probe", (b"10InputProbe", b"16InputProbeLaunch", b"29RecordingHardwareInputRuntime",
                     b"15beginInputProbe", b"17InputProbeReadout", b"19makeInputProbeModel",
                     b"10inputProbe")),
    # The camera without hardware (darwin/platform/synthetic_camera): a source
    # for tests and launchers, which an App Store runtime does not build.
    # namespace uml::platform, its class and entries.
    ("synthetic camera", (b"15SyntheticCamera", b"19SyntheticCameraImpl",
                          b"19makeSyntheticCamera", b"19encodeSyntheticJpeg",
                          b"17decodeChartMarker",
                          # The runtime's choice of it from the environment
                          # (Kernarium/Runtime/KernariumCamera.hpp).
                          b"6camera11parseSource", b"6camera12chooseSource",
                          b"15BackCamerasOnly")),
    # The camera's development lines (darwin/device/camera/camera_diagnostics.hpp):
    # what ROTATE_AND_CROP asks of the host and the footprint at each capture
    # checkpoint, printed by a launcher or a runtime that is not an App Store
    # build, and the Simulator preview's camera and thermal notices
    # (Model.previewCameraNotice and previewThermalNotice).
    ("camera diagnostics", (b"17printRotateAndCrop", b"15printCheckpoint",
                            b"19previewCameraNotice", b"20previewThermalNotice")),
    # Simulated game controllers: the scripted input and the guest end of the
    # gamepad wire (AndariumCoreTestSupport, which only tests and the probe
    # tool link) and the host reader that moved there.
    ("synthetic controllers", (b"13GamepadScript", b"22GamepadHostFrameReader",
                               b"23AndariumCoreTestSupport")),
    # The arm64 JIT engine, the owner of executable memory and the debugger
    # route of the runtime and of the loader. namespace uml::cpu::jit_memory
    # and uml::cpu::arm64::jit, the engine's classes and entry names.
    ("JIT", (b"10jit_memory", b"5arm643jit", b"11JitCompiler", b"11JitExecutor",
             b"9JitEngine", b"16JitCacheGeometry", b"uml_arm64_jit_",
             b"darwin_uml_jit_enter_guest", b"kernarium_runtime_loader_prepare_jit",
             b"kernarium_runtime_loader_debugger_attached",
             # The application's own: the preference, the policy, the
             # debugger side and the Home section (Andarium/Shared/Jit.swift).
             b"13JitPreference", b"8JitShape", b"9JitAnswer", b"9JitStatus",
             b"11JitDebugger", b"10JitSection")),
)
DEBUG_STRINGS = (
    ("frame mirror", (b"uml-frame-tap",)),
    ("footprint log", (b"Kernarium: footprint", b"Kernarium: memory %s")),
    ("virtual Jetsam", (b"virtual Jetsam", b"uml-virtual-jetsam")),
    ("activity log", (b"uml-display %.3f", b"uml-video %.3f")),
    ("guest sampler", (b"# guest code samples",)),
    ("automation", (b"kernarium_runtime_automation_", b"automation boot with",
                    b"ADB authorization is off", b"removed the boot payload")),
    ("remote framebuffer", (b"RFB 003.00",)),
    ("input probe", (b"inputProbeLog", b"inputProbeField")),
    ("camera diagnostics", (b"ROTATE_AND_CROP", b"Camera: memory: ")),
    ("synthetic camera", (b"SyntheticPhone1,1", b"SyntheticTablet1,1",
                          b"SyntheticLaptop1,1", b"synthetic-back-wide",
                          b"KERNARIUM_CAMERA_SOURCE", b"is not a back camera")),
    ("JIT", (b"--jit-cache-budget=", b"--execution-engine=jit",
             b"running on gadgets, not the JIT", b"open Andarium from StikDebug",
             b"Started without JIT")),
)
# What runs tests: a test bundle, and the frameworks and libraries that load it
# (XCTest, XCUIAutomation and the rest, Swift Testing). They are in an
# application that `xcodebuild test` hosted.
TEST_SUPPORT = re.compile(
    r"^(?:.*\.xctest|XC[A-Za-z]*\.framework|libXC[A-Za-z]*\.dylib"
    r"|_?Testing(?:_[A-Za-z0-9]+)?\.framework|lib_?Testing[A-Za-z0-9_]*\.dylib)$")


def debugger_requests(code: bytes, base: int) -> int:
    """How many instruction words of the host's code are the debugger request."""
    count = 0
    for start, end in section_ranges(code, base, CODE_SECTIONS):
        position = code.find(JIT_DEBUGGER_REQUEST, start, end)
        while position != -1:
            count += (position - start) % 4 == 0
            position = code.find(JIT_DEBUGGER_REQUEST, position + 1, end)
    return count


def debug_support(path: Path) -> list[str]:
    """The debugging and automation support in the host code of one Mach-O
    file, as `kind (fragment, ...)` for each kind found."""
    code = host_code(path)
    if code is None:
        return []
    found = []
    for table in (DEBUG_SYMBOLS, DEBUG_STRINGS):
        for kind, fragments in table:
            names = sorted(
                fragment.decode() for fragment in fragments if fragment in code[0])
            if names:
                found.append(f"{kind} ({', '.join(names)})")
    if debugger_requests(*code):
        found.append("JIT (an instruction asking a debugger for JIT memory)")
    return found


JIT_TEXT = re.compile(r"StikDebug|\bJIT\b", re.IGNORECASE)


def strings_in(value: object) -> list[str]:
    """Every key and string value of a property list."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        return [text for key, item in value.items() for text in (strings_in(key) + strings_in(item))]
    if isinstance(value, (list, tuple)):
        return [text for item in value for text in strings_in(item)]
    return []


def localized_jit_text(app: Path) -> list[str]:
    """The JIT text carried as localized text: a JIT strings table of any
    language, and any key or value of another table that names JIT or StikDebug."""
    found = []
    for table in sorted(app.rglob("*.lproj/*")):
        if table.suffix not in (".strings", ".stringsdict"):
            continue
        relative = table.relative_to(app)
        if table.stem == "JIT":
            found.append(f"{relative}: the JIT strings table, which an App Store build does not carry")
            continue
        raw = table.read_bytes()
        try:
            texts = strings_in(plistlib.loads(raw))
        except (plistlib.InvalidFileException, ValueError, OverflowError):
            # An old-style text file: UTF-16 with a mark, or UTF-8.
            texts = [raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", errors="replace")]
        for text in texts:
            if JIT_TEXT.search(text):
                found.append(f"{relative}: carries JIT text ({text[:60]!r})")
                break
    return found


def test_support(app: Path) -> list[str]:
    """The test bundles and test frameworks anywhere in the application."""
    found = []
    for directory, directories, files in os.walk(app):
        for name in list(directories):
            if TEST_SUPPORT.match(name):
                found.append(str((Path(directory) / name).relative_to(app)))
                directories.remove(name)
        found.extend(
            str((Path(directory) / name).relative_to(app))
            for name in files if TEST_SUPPORT.match(name))
    return sorted(found)


def feedback_files(app: Path) -> list[Path]:
    """The Mach-O files that can hold the application's code: its executable,
    the debug library a Debug build keeps the code in, and the frameworks."""
    found = list(executables(app))
    found.extend(path for path in sorted(app.glob("*.dylib")) if path not in found)
    return found


# One strict shape of an address, the same rules as SendFeedbackMail.isWellFormed:
# a local part of 1 to 64 letters, digits and . _ % + - that starts and ends
# with a letter or digit and has no two dots in a row, then @, then at least
# two dot-separated labels of letters, digits and hyphens (no label starts or
# ends with a hyphen) whose last is two or more letters. Quotes, angle
# brackets, a trailing semicolon, spaces and a second @ make no address.
FEEDBACK_ADDRESS_PATTERN = re.compile(
    r"[A-Za-z0-9](?:[A-Za-z0-9._%+-]{0,62}[A-Za-z0-9])?"
    r"@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}")


def feedback_address_shape(address: str) -> bool:
    return (len(address) <= 254 and FEEDBACK_ADDRESS_PATTERN.fullmatch(address) is not None
            and ".." not in address.partition("@")[0])


def feedback_address_problems(configuration: str, address: str) -> list[str]:
    """What is wrong with the address a build names, whatever the product holds.
    The arranged example address is Debug's alone: Release, the build that
    sideloads and forks make, never takes one."""
    if not address:
        return []
    if configuration != "AppStore":
        if address != FEEDBACK_ARRANGED:
            return [f"Send feedback: {configuration} names a receiver that is not the arranged example address; "
                    "only the App Store configuration reads the local configuration"]
        if configuration != "Debug":
            return [f"Send feedback: {configuration} names the arranged example address; "
                    "only Debug takes the arrangement"]
        return []
    if not feedback_address_shape(address):
        return ["Send feedback: the receiver in the local configuration is not an email address"]
    domain = address.partition("@")[2].lower()
    if (any(domain == name or domain.endswith("." + name) for name in PLACEHOLDER_DOMAINS)
            or domain.rpartition(".")[2] in PLACEHOLDER_TLDS):
        return ["Send feedback: the receiver in the local configuration is a placeholder address"]
    return []


def receiver_leaks(app: Path, receiver: str) -> list[str]:
    """The files of the bundle whose bytes hold the receiver of the local
    configuration, in either case; named by path, never by the receiver."""
    needle = receiver.lower().encode()
    found = []
    for directory, _directories, files in os.walk(app):
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                continue
            tail = b""
            try:
                with path.open("rb") as handle:
                    while chunk := handle.read(8 << 20):
                        window = (tail + chunk).lower()
                        if needle in window:
                            found.append(str(path.relative_to(app)))
                            break
                        tail = window[-(len(needle) - 1):] if len(needle) > 1 else b""
            except OSError:
                continue
    return sorted(found)


# The optional features' blocks of Config/Info.plist, and the keys they write.
OPTIONAL_BLOCKS = re.compile(r"^#ifdef (?:FEEDBACK_MAIL|FOLLOW_X)\n.*?^#endif\n", re.S | re.M)
OPTIONAL_KEYS = frozenset({FEEDBACK_KEY, FOLLOW_X_KEY})


def info_source_problems(app: Path, source: Path) -> list[str]:
    """Every key of Config/Info.plist but the optional features' (feedback,
    Follow developer on X), as the built Info.plist has it. The source is
    preprocessed (those blocks are conditional), and a build adds keys of its own and fills in build
    settings, so what is compared is each source key, a value naming a build
    setting (`$(...)`) matching whatever the build made of it. A preprocessor
    that expanded a macro in a value shows here."""
    try:
        text = OPTIONAL_BLOCKS.sub("", source.read_text(encoding="utf-8"))
        wanted = plistlib.loads(text.encode())
        with (app / "Info.plist").open("rb") as handle:
            built = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        return [f"Info.plist: cannot be compared with {source.name}"]

    def same(expected: object, actual: object) -> bool:
        if isinstance(expected, str) and "$(" in expected:
            return isinstance(actual, str)
        if isinstance(expected, dict):
            return (isinstance(actual, dict)
                    and all(key in actual and same(value, actual[key]) for key, value in expected.items()))
        if isinstance(expected, list):
            return (isinstance(actual, list) and len(actual) == len(expected)
                    and all(same(left, right) for left, right in zip(expected, actual)))
        return type(expected) is type(actual) and expected == actual

    found = []
    for key, value in wanted.items():
        if key in OPTIONAL_KEYS:
            continue
        if key not in built:
            found.append(f"Info.plist: {key} of {source.name} is missing from the built one")
        elif not same(value, built[key]):
            found.append(f"Info.plist: {key} differs from {source.name}")
    return found


def feedback_strings(app: Path) -> list[str]:
    """The String Catalog tables of the application that mention Send feedback."""
    found = []
    for path in sorted(app.glob("*.lproj/*.strings")):
        try:
            with path.open("rb") as handle:
                table = plistlib.load(handle)
        except (OSError, plistlib.InvalidFileException, ValueError):
            continue
        if isinstance(table, dict) and any(
                word in text for key, value in table.items() for text in (str(key), str(value))
                for word in FEEDBACK_WORDS):
            found.append(str(path.relative_to(app)))
    return found


def feedback_tables(app: Path) -> list[Path]:
    """The Feedback table of any language, found by its file name."""
    return sorted(path for path in app.glob("*.lproj/*")
                  if path.stem == FEEDBACK_TABLE and path.suffix in (".strings", ".stringsdict"))


def feedback_problems(
    app: Path, configuration: str, address: str, receiver: str = "",
) -> tuple[list[str], bool]:
    """Whether the product holds Send feedback as the build said it would, and
    whether it holds it. `address` is the build setting FEEDBACK_ADDRESS: the
    local configuration's receiver in the App Store configuration, empty
    otherwise, or the example address a test or capture script arranged.
    `receiver` is the local configuration's own receiver, whatever the
    configuration: wherever the build does not name it, no file of the
    bundle holds it."""
    problems = feedback_address_problems(configuration, address)
    expected = bool(address) and not problems
    if receiver and (not expected or receiver != address):
        problems.extend(f"{name}: holds the receiver of the local configuration"
                        for name in receiver_leaks(app, receiver))
    fragments: dict[str, set[bytes]] = {}
    for binary in feedback_files(app):
        code = host_code(binary)
        if code is None:
            continue
        # The linker's debug map names the object and source files of the
        # build, which hold the feature's file names though it is compiled
        # out: the check blanks those paths and reads what the code and its
        # symbols hold, among them a symbol that a function stab shares.
        data = without_debug_map(*code, PATH_STABS)
        present = {fragment for fragment in FEEDBACK_FRAGMENTS if fragment in data}
        if present:
            fragments[str(binary.relative_to(app))] = present
    try:
        with (app / "Info.plist").open("rb") as handle:
            keyed = plistlib.load(handle).get(FEEDBACK_KEY)
    except (OSError, plistlib.InvalidFileException, ValueError):
        keyed = None
    words = feedback_strings(app)
    tables = feedback_tables(app)
    compiled = bool(fragments) or keyed is not None or bool(words) or bool(tables)
    if not expected:
        if address:
            problems.append("Send feedback: the build names a receiver that cannot be used")
        for name, present in fragments.items():
            problems.append(
                f"{name}: carries Send feedback ({', '.join(sorted(item.decode() for item in present))}) "
                "though the build names no receiver")
        if keyed is not None:
            problems.append(f"Info.plist: has {FEEDBACK_KEY} though the build names no receiver")
        problems.extend(f"{name}: has Send feedback strings though the build names no receiver"
                        for name in words)
        problems.extend(f"{path.relative_to(app)}: is the Feedback strings table though the build names no receiver"
                        for path in tables)
        return problems, compiled
    if keyed != address:
        problems.append(f"Info.plist: {FEEDBACK_KEY} must be the receiver the build names")
    code = set().union(*fragments.values()) if fragments else set()
    missing = [item.decode() for item in FEEDBACK_REQUIRED if item not in code]
    if missing:
        problems.append(
            f"the application lacks Send feedback ({', '.join(missing)}) though the build names a receiver")
    absent = [language for language in FEEDBACK_LANGUAGES
              if not (app / f"{language}.lproj" / f"{FEEDBACK_TABLE}.strings").is_file()]
    if absent:
        problems.append(
            f"the application lacks the Feedback strings table in {', '.join(absent)} though the build names a receiver")
    return problems, compiled


def follow_x_handle_problems(configuration: str, handle: str) -> list[str]:
    """What is wrong with the handle a build names, whatever the product holds.
    The arranged placeholder handle is Debug's alone: Release, the build that
    sideloads and forks make, never takes one."""
    if not handle:
        return []
    if configuration != "AppStore":
        if handle != FOLLOW_X_ARRANGED:
            return [f"Follow developer on X: {configuration} names a handle that is not the arranged placeholder; "
                    "only the App Store configuration reads the local configuration"]
        if configuration != "Debug":
            return [f"Follow developer on X: {configuration} names the arranged placeholder handle; "
                    "only Debug takes the arrangement"]
        return []
    if FOLLOW_X_HANDLE_PATTERN.fullmatch(handle) is None:
        return ["Follow developer on X: the handle in the local configuration is not a handle of X"]
    if handle.lower() == FOLLOW_X_ARRANGED:
        return ["Follow developer on X: the handle in the local configuration is the placeholder handle"]
    return []


def follow_x_leaks(app: Path, handle: str) -> list[str]:
    """The files of the bundle that hold the local configuration's handle as
    a profile address (in either case, in any file), or as the whole of a
    string of a property list or strings table. The bare word is no leak:
    the bundle identifier holds the same one. Named by path, never by the
    handle."""
    needles = [f"{host}/{handle}".lower().encode() for host in FOLLOW_X_HOSTS]
    longest = max(len(needle) for needle in needles)
    wanted = handle.lower()
    found = []
    for directory, _directories, files in os.walk(app):
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                continue
            relative = str(path.relative_to(app))
            try:
                tail = b""
                with path.open("rb") as stream:
                    while chunk := stream.read(8 << 20):
                        window = (tail + chunk).lower()
                        if any(needle in window for needle in needles):
                            found.append(relative)
                            break
                        tail = window[-(longest - 1):]
                    else:
                        if path.suffix in (".plist", ".strings", ".stringsdict") and path.stat().st_size <= 16 << 20:
                            try:
                                texts = strings_in(plistlib.loads(path.read_bytes()))
                            except (plistlib.InvalidFileException, ValueError, OverflowError):
                                texts = []
                            if any(text.lower() == wanted for text in texts):
                                found.append(relative)
            except OSError:
                continue
    return sorted(found)


def follow_x_tables(app: Path) -> list[Path]:
    """The FollowX table of any language, found by its file name."""
    return sorted(path for path in app.glob("*.lproj/*")
                  if path.stem == FOLLOW_X_TABLE and path.suffix in (".strings", ".stringsdict"))


def follow_x_words(app: Path) -> list[str]:
    """The String Catalog tables of the application that mention the row."""
    found = []
    for path in sorted(app.glob("*.lproj/*.strings")):
        try:
            with path.open("rb") as handle:
                table = plistlib.load(handle)
        except (OSError, plistlib.InvalidFileException, ValueError):
            continue
        if isinstance(table, dict) and any(
                word in text for key, value in table.items() for text in (str(key), str(value))
                for word in FOLLOW_X_WORDS):
            found.append(str(path.relative_to(app)))
    return found


def follow_x_logo(app: Path) -> tuple[bool, list[str]]:
    """Whether the application's asset catalog holds the logo, and the artwork
    files lying in the bundle (the catalog is where the logo belongs)."""
    try:
        compiled = FOLLOW_X_ASSET in (app / "Assets.car").read_bytes()
    except OSError:
        compiled = False
    loose = sorted(
        str((Path(directory) / name).relative_to(app))
        for directory, _directories, files in os.walk(app) for name in files if name in FOLLOW_X_FILES)
    return compiled, loose


def follow_x_problems(
    app: Path, configuration: str, handle: str, local_handle: str = "",
) -> tuple[list[str], bool]:
    """Whether the product holds Follow developer on X as the build said it
    would, and whether it holds it. `handle` is the build setting
    FOLLOW_X_HANDLE: the local configuration's handle in the App Store
    configuration, empty otherwise, or the placeholder a test or capture
    script arranged. `local_handle` is the local configuration's own handle,
    whatever the configuration: wherever the build does not name it, no file
    of the bundle holds its profile address. The arranged placeholder is held
    to the same rule, by its bytes, which only it has: it is in no product
    that does not arrange it."""
    problems = follow_x_handle_problems(configuration, handle)
    expected = bool(handle) and not problems
    if local_handle and (not expected or local_handle != handle):
        problems.extend(f"{name}: holds the handle of the local configuration"
                        for name in follow_x_leaks(app, local_handle))
    if handle != FOLLOW_X_ARRANGED or not expected:
        problems.extend(f"{name}: holds the placeholder handle of the arrangement"
                        for name in receiver_leaks(app, FOLLOW_X_ARRANGED))
    fragments: dict[str, set[bytes]] = {}
    for binary in feedback_files(app):
        code = host_code(binary)
        if code is None:
            continue
        data = without_debug_map(*code, PATH_STABS)
        present = {fragment for fragment in FOLLOW_X_FRAGMENTS if fragment in data}
        if present:
            fragments[str(binary.relative_to(app))] = present
    try:
        with (app / "Info.plist").open("rb") as stream:
            keyed = plistlib.load(stream).get(FOLLOW_X_KEY)
    except (OSError, plistlib.InvalidFileException, ValueError):
        keyed = None
    words = follow_x_words(app)
    tables = follow_x_tables(app)
    logo, loose = follow_x_logo(app)
    compiled = bool(fragments) or keyed is not None or bool(words) or bool(tables) or logo
    problems.extend(f"{name}: is artwork of the row, which belongs in the asset catalog" for name in loose)
    if not expected:
        if handle:
            problems.append("Follow developer on X: the build names a handle that cannot be used")
        for name, present in fragments.items():
            problems.append(
                f"{name}: carries Follow developer on X ({', '.join(sorted(item.decode() for item in present))}) "
                "though the build names no handle")
        if keyed is not None:
            problems.append(f"Info.plist: has {FOLLOW_X_KEY} though the build names no handle")
        problems.extend(f"{name}: has Follow developer on X strings though the build names no handle"
                        for name in words)
        problems.extend(f"{path.relative_to(app)}: is the FollowX strings table though the build names no handle"
                        for path in tables)
        if logo:
            problems.append("Assets.car: holds the logo of Follow developer on X though the build names no handle")
        return problems, compiled
    if keyed != handle:
        problems.append(f"Info.plist: {FOLLOW_X_KEY} must be the handle the build names")
    code = set().union(*fragments.values()) if fragments else set()
    missing = [item.decode() for item in FOLLOW_X_REQUIRED if item not in code]
    if missing:
        problems.append(
            f"the application lacks Follow developer on X ({', '.join(missing)}) though the build names a handle")
    absent = [language for language in FEEDBACK_LANGUAGES
              if not (app / f"{language}.lproj" / f"{FOLLOW_X_TABLE}.strings").is_file()]
    if absent:
        problems.append(
            f"the application lacks the FollowX strings table in {', '.join(absent)} though the build names a handle")
    if not logo:
        problems.append("Assets.car: lacks the logo of Follow developer on X though the build names a handle")
    return problems, compiled


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("app", type=Path)
    parser.add_argument("--privacy-checker", type=Path, default=CHECKER)
    parser.add_argument(
        "--configuration", default="AppStore",
        help="the build configuration; Send feedback is checked against it")
    parser.add_argument(
        "--feedback-address", default="",
        help="the build setting FEEDBACK_ADDRESS: the receiver the build names, "
             "empty when it names none")
    parser.add_argument(
        "--feedback-receiver", default="",
        help="the build setting FEEDBACK_RECEIVER: the local configuration's receiver, "
             "which no file of a bundle that does not name it may hold")
    parser.add_argument(
        "--info-source", type=Path, default=None,
        help="Config/Info.plist, whose values the built Info.plist keeps")
    parser.add_argument(
        "--follow-x-handle", default="",
        help="the build setting FOLLOW_X_HANDLE: the handle the build names, "
             "empty when it names none")
    parser.add_argument(
        "--developer-x-handle", default="",
        help="the build setting DEVELOPER_X_HANDLE: the local configuration's handle, "
             "whose profile address no file of a bundle that does not name it may hold")
    parser.add_argument(
        "--feedback", action="store_true",
        help="check Send feedback alone, for a configuration other than AppStore")
    parser.add_argument(
        "--follow-x", action="store_true",
        help="check Follow developer on X alone, for a configuration other than AppStore "
             "(with --feedback, the two)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--runtime", action="store_true",
        help="the path is one runtime framework: check its automation knobs, "
             "its debugging support and the traces of its build host")
    mode.add_argument(
        "--host-traces", action="store_true",
        help="the path is one framework: check the traces of its build host alone")
    options = parser.parse_args()

    app = options.app
    if options.feedback or options.follow_x:
        found = []
        if options.info_source is not None:
            found.extend(info_source_problems(app, options.info_source))
        lines = []
        if options.feedback:
            more, compiled = feedback_problems(
                app, options.configuration, options.feedback_address, options.feedback_receiver)
            found.extend(more)
            lines.append(f"{app.name}: Send feedback is {'compiled in' if compiled else 'not compiled in'}"
                         f" ({options.configuration})")
        if options.follow_x:
            more, compiled = follow_x_problems(
                app, options.configuration, options.follow_x_handle, options.developer_x_handle)
            found.extend(more)
            lines.append(f"{app.name}: Follow developer on X is {'compiled in' if compiled else 'not compiled in'}"
                         f" ({options.configuration})")
        for line in found:
            print(line, file=sys.stderr)
        for line in lines:
            print(line)
        return 1 if found else 0
    if options.runtime or options.host_traces:
        found = []
        for binary in executables_of(app):
            if options.runtime:
                found.extend(f"{binary.name}: carries automation knob {name}" for name in knobs(binary))
                found.extend(f"{binary.name}: carries debugging support: {item}"
                             for item in debug_support(binary))
            found.extend(f"{binary.name}: {trace}" for trace in build_host_traces(binary))
        for line in found:
            print(line, file=sys.stderr)
        return 1 if found else 0
    problems: list[str] = []
    try:
        with (app / "Info.plist").open("rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        print(f"{app}: has no readable Info.plist", file=sys.stderr)
        return 1
    problems.extend(f"{app.name}: {problem}" for problem in info_problems(info))

    for binary in executables(app):
        names = knobs(binary)
        if names:
            problems.append(
                f"{binary.relative_to(app)}: carries automation knob(s) "
                f"{', '.join(names[:6])}{' ...' if len(names) > 6 else ''}")
        lines = harness_lines(binary)
        if lines:
            problems.append(
                f"{binary.relative_to(app)}: carries the harness's console line(s) "
                f"{', '.join(repr(line) for line in lines)}")
        problems.extend(
            f"{binary.relative_to(app)}: carries debugging support: {item}"
            for item in debug_support(binary))
        # The application's own executable is stripped when Xcode archives it.
        problems.extend(
            f"{binary.relative_to(app)}: {trace}"
            for trace in build_host_traces(binary, debug_map=binary.parent != app))

    problems.extend(f"{app.name}: {text}" for text in localized_jit_text(app))

    problems.extend(
        f"{path}: a test bundle or test framework; build the application "
        "without its tests" for path in test_support(app))

    if options.info_source is not None:
        problems.extend(info_source_problems(app, options.info_source))
    feedback, compiled = feedback_problems(
        app, options.configuration, options.feedback_address, options.feedback_receiver)
    problems.extend(feedback)
    print(f"{app.name}: Send feedback is {'compiled in' if compiled else 'not compiled in'}")
    follow, compiled = follow_x_problems(
        app, options.configuration, options.follow_x_handle, options.developer_x_handle)
    problems.extend(follow)
    print(f"{app.name}: Follow developer on X is {'compiled in' if compiled else 'not compiled in'}")

    privacy = subprocess.run(
        [sys.executable, "-B", str(options.privacy_checker), str(app)],
        check=False, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, timeout=600)
    if privacy.returncode != 0:
        problems.extend(line for line in privacy.stderr.splitlines() if line)

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"{app.name}: ready for the App Store")
    return 0


if __name__ == "__main__":
    sys.exit(main())
