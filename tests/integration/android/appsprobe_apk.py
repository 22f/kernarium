# SPDX-License-Identifier: GPL-2.0-or-later
"""The applications that `run_android_app_shortcuts.py` installs, built without Android's SDK.

The launcher-application command (android/lineage-24/overlay/app_shortcuts) lists the activities that handle
MAIN with the LAUNCHER category, with their labels, and starts one. Each activity of these packages is a
framework class (see orientation_probe_apk.py: the package has no code of its own), so a manifest is the whole
application, and the manifest is what the command reads:

- ``first`` has four activities: ``listed`` (a launcher activity whose label holds a TAB, a line break, a
  right-to-left override and a zero-width space, which the listing must escape), ``long`` (a launcher activity
  whose label is longer than the listing's bound), ``hidden`` (a launcher activity that is not exported, which
  the command must neither list nor start) and ``plain`` (exported, with no launcher filter, which the command
  must not start either). The package declares no icon, so the icon of its activities is ``noicon``;
- ``second`` has one launcher activity and is installed and removed to change the set of applications;
- ``first-v2`` is ``first`` with a higher version code, signed with the same key: an update in place.

The runner's tooling test reads the manifests back with ``orientation_probe_apk.decode_manifest``.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib

import orientation_probe_apk as binary_xml

PACKAGE_PREFIX = "org.example.andarium.appsprobe"
VARIANTS = ("first", "second")
SUCCESSORS = ("first-v2",)

#: The framework classes the activities are, one for each activity of a package: a class that needs no code.
CLASSES = {
    "listed": "android.app.Activity",
    "long": "android.app.TabActivity",
    "hidden": "android.app.ListActivity",
    "plain": "android.app.ExpandableListActivity",
    "only": "android.app.Activity",
}

#: Labels as the manifest holds them (BMP characters only: the string pool counts UTF-16 units)...
HOSTILE_LABEL = "Apps\tProbe\nName‮ Evil​  Label "
LONG_LABEL = "Long" + "x" * 196
SECOND_LABEL = "Second Probe"
#: ...and what the listing must show for them.
LISTED_HOSTILE_LABEL = "Apps Probe Name Evil Label"
LISTED_LONG_LABEL = "Long" + "x" * 75 + "…"
LISTED_SECOND_LABEL = SECOND_LABEL


def package_of(variant: str) -> str:
    return f"{PACKAGE_PREFIX}.{variant.split('-')[0]}"


def activity_names(variant: str) -> tuple[str, ...]:
    """The activities a variant's manifest declares."""
    return ("listed", "long", "hidden", "plain") if variant.startswith("first") else ("only",)


def component(variant: str, activity: str) -> str:
    """The component token (package/class) of one of a variant's activities."""
    return f"{package_of(variant)}/{CLASSES[activity]}"


def launcher_filter() -> binary_xml.Element:
    return binary_xml.Element("intent-filter", (), (
        binary_xml.Element("action", (("name", "android.intent.action.MAIN"),)),
        binary_xml.Element("category", (("name", "android.intent.category.LAUNCHER"),)),
    ))


def manifest_tree(variant: str) -> binary_xml.Element:
    version = 2 if variant.endswith("-v2") else 1
    activities: list[binary_xml.Element] = []
    for activity in activity_names(variant):
        attributes: list[tuple[str, str | int | bool]] = [("name", CLASSES[activity])]
        attributes.append(("exported", activity != "hidden"))
        label = {"listed": HOSTILE_LABEL, "long": LONG_LABEL, "only": SECOND_LABEL}.get(activity)
        if label is not None:
            attributes.append(("label", label))
        has_filter = activity != "plain"
        activities.append(binary_xml.Element("activity", tuple(attributes), (launcher_filter(),) if has_filter else ()))
    return binary_xml.Element("manifest", (("package", package_of(variant)), ("versionCode", version)), (
        binary_xml.Element("uses-sdk", (("minSdkVersion", binary_xml.MIN_SDK),
                                        ("targetSdkVersion", binary_xml.TARGET_SDK))),
        binary_xml.Element("application", (("label", "Apps Probe Application"), ("hasCode", False)),
                           tuple(activities)),
    ))


def build_apk(destination: pathlib.Path, variant: str, keystore: pathlib.Path | None = None) -> pathlib.Path:
    """Writes the signed package of a variant. The variants of one package (``first`` and ``first-v2``) are signed with
    one key, so that the second installs over the first in place: name the same `keystore` for both."""
    return binary_xml.build_apk(destination, tree=manifest_tree(variant), keystore=keystore)
