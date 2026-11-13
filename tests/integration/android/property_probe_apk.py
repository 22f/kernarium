# SPDX-License-Identifier: GPL-2.0-or-later
"""An application that only lets the host run a command as it, built without Android's SDK.

run_android_installation_identity.py asks what an ordinary application can read of the guest's
properties: `adb shell run-as <package> getprop <name>` runs getprop in the application's own
uid and SELinux domain (untrusted_app), which is the reader the question is about. run-as
needs a debuggable package, and a package with no code and no component is the whole
application. Its manifest is Android's binary XML (orientation_probe_apk.encode_manifest) and
the JAR signature is a throwaway key's, as for the orientation probe, so the result belongs to
no one and targets API 29, which every release since 14 installs.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib

import orientation_probe_apk as binary_xml

PACKAGE = "org.example.andarium.propertyprobe"


def manifest_tree() -> binary_xml.Element:
    """A debuggable application with no code and no component."""
    return binary_xml.Element("manifest", (("package", PACKAGE), ("versionCode", 1)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", binary_xml.MIN_SDK),
                                        ("targetSdkVersion", binary_xml.TARGET_SDK))),
        binary_xml.Element("application", (("label", "Property Probe"), ("hasCode", False),
                                           ("debuggable", True))),
    ))


def build_apk(destination: pathlib.Path, workdir: pathlib.Path | None = None) -> pathlib.Path:
    return binary_xml.build_apk(destination, workdir, manifest_tree())
