# SPDX-License-Identifier: GPL-2.0-or-later
"""The applications that `run_android_oldapp_switch.py` installs, built without Android's SDK.

The Old app compatibility command (android/lineage-24/overlay/oldapp_switch) lists the applications the
compatibility layer can apply to and sets the persistent property of one. An eligible application is not a
system application, has a 32-bit primary ABI, targets below API 14 and has no shared user ID. The runner
needs one eligible application and, for each clause of the rule, one that fails only that clause:

- ``old``: targetSdkVersion 10 with a native library for armeabi-v7a. Eligible. Its label holds a TAB, which
  the listing must turn into a space (the label is the manifest's own string, so the listing's resource
  path is not exercised: `--real-apk` of the runner installs an application whose label is a resource);
- ``modern``: the same with targetSdkVersion 29, the highest a host can sign without Android's tools (the JAR
  signature of orientation_probe_apk.py is scheme v1). Not eligible by its target level. It also carries the
  receiver of property_probe_dex.py with a script of its own: `am broadcast` makes the zygote fork an ordinary
  application process (`untrusted_app_29`) that reads the eligible application's property and tries to set it;
  the package is debuggable, so the runner reads the result with ``run-as``;
- ``javaonly``: targetSdkVersion 10 without native code, so its primary ABI is the 64-bit one. Not eligible by
  its ABI;
- ``shared``: targetSdkVersion 10 with a native library and a shared user ID. Not eligible by that.

The native library is a stand-in: a valid ELF32 ARM shared object that holds nothing, because only the
package manager reads it (the primary ABI follows the directory it is in) and no application loads it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import struct

import orientation_probe_apk as binary_xml
import property_probe_dex

PACKAGE_PREFIX = "org.example.andarium.oldswitchprobe"
STORE_PROPERTY_PREFIX = "persist.sys.kernarium.oldapp."
LABEL = "Old Switch\tProbe"
#: What the command lists for LABEL: the TAB becomes a space.
LISTED_LABEL = "Old Switch Probe"
RECEIVER = property_probe_dex.CLASS_NAME
#: Relative to the application's data directory, where `run-as` starts.
RESULT_FILE = "files/result"
LIBRARY = "lib/armeabi-v7a/libkoldswitch.so"

#: variant -> (package, minSdkVersion, targetSdkVersion, has the native library, shared user ID, probe receiver)
VARIANTS = {
    "old": (PACKAGE_PREFIX, 4, 10, True, None, False),
    "modern": (PACKAGE_PREFIX + ".modern", 21, 29, True, None, True),
    "javaonly": (PACKAGE_PREFIX + ".javaonly", 4, 10, False, None, False),
    "shared": (PACKAGE_PREFIX + ".shared", 4, 10, True, PACKAGE_PREFIX + ".shareduid", False),
}
#: The variants the command must list, and the ones it must refuse.
ELIGIBLE = ("old",)
REFUSED = ("modern", "javaonly", "shared")


def package_of(variant: str) -> str:
    return VARIANTS[variant][0]


def target_of(variant: str) -> int:
    return VARIANTS[variant][2]


def property_of(variant: str) -> str:
    return STORE_PROPERTY_PREFIX + package_of(variant)


def component_of(variant: str) -> str:
    return f"{package_of(variant)}/{RECEIVER}"


def stand_in_library() -> bytes:
    """An ELF32 little-endian ARM shared object with a PT_LOAD and a PT_DYNAMIC that holds only DT_NULL, aligned to
    16 KiB: a file the package manager takes for a library and the loader would take for an empty one."""
    alignment = 0x4000
    header_size, program_header_size, count = 52, 32, 2
    dynamic_offset = header_size + count * program_header_size
    dynamic = struct.pack("<II", 0, 0)
    size = dynamic_offset + len(dynamic)
    header = (b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
              + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, header_size, 0, 0x05000200, header_size,
                            program_header_size, count, 40, 0, 0))
    load = struct.pack("<IIIIIIII", 1, 0, 0, 0, size, size, 4, alignment)
    dynamic_header = struct.pack("<IIIIIIII", 2, dynamic_offset, dynamic_offset, dynamic_offset, len(dynamic),
                                 len(dynamic), 6, 4)
    image = header + load + dynamic_header + dynamic
    assert len(image) == size, (len(image), size)
    return image


def probe_script(package: str, watched_property: str) -> str:
    """What the modern package's shell runs as that application: the eligible application's property as it reads
    it, whether setting it is allowed, and the value afterwards (the value is not secret: the runner chose it)."""
    directory = f"/data/data/{package}/files"
    return (
        f"d={directory}; mkdir -p $d; "
        "{ "
        'echo "context=$(id -Z)"; '
        f'echo "get=$(getprop {watched_property})"; '
        f"if setprop {watched_property} 7 2>/dev/null; then echo set=allowed; else echo set=denied; fi; "
        f'echo "after=$(getprop {watched_property})"; '
        "echo done; "
        "} > $d/result.tmp && mv $d/result.tmp $d/result")


def manifest_tree(variant: str) -> binary_xml.Element:
    package, minimum, target, _, shared, probe = VARIANTS[variant]
    attributes: tuple[tuple[str, str | int | bool], ...] = (("package", package), ("versionCode", 1))
    if shared is not None:
        attributes += (("sharedUserId", shared),)
    application = [("label", LABEL if variant == "old" else f"Switch Probe {variant}")]
    children: tuple[binary_xml.Element, ...] = ()
    if probe:
        application.append(("debuggable", True))
        children = (binary_xml.Element("receiver", (("name", RECEIVER), ("exported", True))),)
    return binary_xml.Element("manifest", attributes, (
        binary_xml.Element("uses-sdk", (("minSdkVersion", minimum), ("targetSdkVersion", target))),
        binary_xml.Element("application", tuple(application), children),
    ))


def build_apk(destination: pathlib.Path, variant: str, workdir: pathlib.Path | None = None) -> pathlib.Path:
    """The signed package of a variant at `destination`."""
    _, _, _, native, _, probe = VARIANTS[variant]
    files: dict[str, bytes] = {}
    if native:
        files[LIBRARY] = stand_in_library()
    if probe:
        files["classes.dex"] = property_probe_dex.build_dex(
            package_of(variant), property_of("old"), property_of("old"),
            shell_script=probe_script(package_of(variant), property_of("old")))
    return binary_xml.build_apk(destination, workdir, manifest_tree(variant), files)
