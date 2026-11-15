#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The activity that replaces the sensor privacy dialog of a blocked camera: the sources agree.

A session started with Camera off reports the camera lens cover, the sensor privacy service takes it for a
hardware toggle, and it then starts the activity that the framework resource
``config_sensorUseStartedActivity_hwToggle`` names over the application that used the camera, without a try
block. SystemUI's dialog (the default) offers an Unblock that cannot lift a hardware toggle, so the product
names an activity of its own that shows nothing (``overlay/apps/KernariumSensorUseQuiet``), and a name that
the image does not hold would end system_server. This test reads the sources that must agree, without an
Android build: the framework overlay's string, the application's manifest, class and build rule, the product
makefile, the build script's assertion and the constants of the target-files verifier (whose fixtures show
that the verifier rejects the image that disagrees). It then changes each source in a copy and requires the
check to fail, so that a rule that reads nothing cannot pass.

usage: test_sensor_use_contract.py --source-root DIR
"""

from __future__ import annotations
# Standalone tests must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import re
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET

ANDROID = "android/lineage-24"
MODULE = "KernariumSensorUseQuiet"
MODULE_DIRECTORY = f"{ANDROID}/overlay/apps/{MODULE}"
FRAMEWORK_CONFIG = f"{ANDROID}/overlay/overlay/frameworks/base/core/res/res/values/config.xml"
PRODUCT_MAKEFILE = f"{ANDROID}/overlay/lineage_andarium.mk"
BUILD_SCRIPT = f"{ANDROID}/build/build-aosp.sh"
VERIFIER = f"{ANDROID}/tools/verify-target-files.py"
ANDROID_NAMESPACE = "{http://schemas.android.com/apk/res/android}"
SOURCES = (
    FRAMEWORK_CONFIG, PRODUCT_MAKEFILE, BUILD_SCRIPT, f"{MODULE_DIRECTORY}/Android.bp",
    f"{MODULE_DIRECTORY}/AndroidManifest.xml",
)


class ContractError(Exception):
    pass


def load_verifier(root: pathlib.Path):
    spec = importlib.util.spec_from_file_location("verify_target_files", root / VERIFIER)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check(root: pathlib.Path, verifier) -> None:
    """Raises ContractError naming the first disagreement."""
    package, activity = verifier.SENSOR_USE_PACKAGE, verifier.SENSOR_USE_ACTIVITY
    component = f"{package}/{activity}"
    if not activity.startswith(package + "."):
        raise ContractError(f"the verifier's activity {activity} is not in its package {package}")

    # The framework overlay names the activity, for the hardware toggle only, and leaves the rest of the
    # sensor privacy configuration as the platform has it: no software camera toggle, no other activity.
    config = ET.parse(root / FRAMEWORK_CONFIG).getroot()
    strings = {element.get("name"): (element.text or "").strip() for element in config.iter("string")}
    if strings.get(verifier.SENSOR_USE_HW_TOGGLE_ACTIVITY) != component:
        raise ContractError(f"{FRAMEWORK_CONFIG} must set {verifier.SENSOR_USE_HW_TOGGLE_ACTIVITY} to "
                            f"{component}: {strings.get(verifier.SENSOR_USE_HW_TOGGLE_ACTIVITY)!r}")
    for name in ("config_sensorUseStartedActivity",):
        if name in strings:
            raise ContractError(f"{FRAMEWORK_CONFIG} sets {name}: the software toggle's dialog stays the "
                                "platform's, and a live session never reaches the hardware one")
    booleans = {element.get("name"): (element.text or "").strip() for element in config.iter("bool")}
    for name in ("config_supportsCamToggle", "config_supportsHardwareMicToggle", "config_supportsMicToggle"):
        if name in booleans:
            raise ContractError(f"{FRAMEWORK_CONFIG} sets {name}: this change adds no sensor toggle")
    if booleans.get(verifier.CAMERA_HARDWARE_TOGGLE) != "true":
        raise ContractError(f"{FRAMEWORK_CONFIG} no longer sets {verifier.CAMERA_HARDWARE_TOGGLE} to true")

    # The application: package, activity, not exported, no display, direct-boot aware, and a class that ends
    # in onCreate (a Theme.NoDisplay activity must finish before it resumes).
    manifest = ET.parse(root / MODULE_DIRECTORY / "AndroidManifest.xml").getroot()
    if manifest.get("package") != package:
        raise ContractError(f"the manifest's package {manifest.get('package')!r} is not {package}")
    application = manifest.find("application")
    if application is None or application.get(ANDROID_NAMESPACE + "directBootAware") != "true":
        raise ContractError("the application must be directBootAware: a user whose storage is locked "
                            "resolves only such components, and the service starts this one without a try block")
    activities = application.findall("activity")
    if len(activities) != 1 or activities[0].get(ANDROID_NAMESPACE + "name") != activity:
        raise ContractError(f"the manifest must declare the one activity {activity}")
    declared = activities[0]
    if declared.get(ANDROID_NAMESPACE + "exported") != "false":
        raise ContractError("the activity must not be exported: the system uid starts it and nothing else may")
    if declared.get(ANDROID_NAMESPACE + "theme") != "@android:style/Theme.NoDisplay":
        raise ContractError("the activity must use @android:style/Theme.NoDisplay")
    if list(declared) or list(application.findall("service")) or list(application.findall("receiver")):
        raise ContractError("the application declares an intent filter, service or receiver: it only ends")
    if manifest.findall("uses-permission"):
        raise ContractError("the application requests a permission: it holds none")
    source = root / MODULE_DIRECTORY / "src" / pathlib.PurePosixPath(*activity.split(".")).with_suffix(".java")
    text = source.read_text() if source.is_file() else ""
    simple_name = activity.rsplit(".", 1)[1]
    if (not re.search(rf"^package {re.escape(package)};$", text, re.M)
            or not re.search(rf"class {simple_name} extends Activity\b", text)
            or not re.search(r"protected void onCreate\(Bundle [A-Za-z]+\) \{[^}]*\bfinish\(\);", text)):
        raise ContractError(f"{source.relative_to(root)} must declare {activity} as an Activity that calls "
                            "finish() in onCreate")
    if "SPDX-License-Identifier: Apache-2.0" not in text:
        raise ContractError(f"{source.relative_to(root)} lacks the Apache-2.0 header of Android-tree code")

    # The build rule installs it in the product partition where the verifier looks, and the product and the
    # build script both name the module.
    rule = (root / MODULE_DIRECTORY / "Android.bp").read_text()
    expected_apk = f"PRODUCT/app/{MODULE}/{MODULE}.apk"
    if verifier.SENSOR_USE_APK != expected_apk or expected_apk not in verifier.REQUIRED_ENTRIES:
        raise ContractError(f"the verifier must require {expected_apk}")
    for needle in (f'name: "{MODULE}"', "product_specific: true,", 'certificate: "platform",',
                   'sdk_version: "current",', 'manifest: "AndroidManifest.xml",'):
        if needle not in rule:
            raise ContractError(f"{MODULE_DIRECTORY}/Android.bp lacks {needle}")
    product = (root / PRODUCT_MAKEFILE).read_text()
    if len(re.findall(rf"^PRODUCT_PACKAGES \+= {MODULE}$", product, re.M)) != 1:
        raise ContractError(f"lineage_andarium.mk does not add {MODULE} to PRODUCT_PACKAGES exactly once")
    if f'*" {MODULE} "*' not in (root / BUILD_SCRIPT).read_text():
        raise ContractError(f"build-aosp.sh does not assert that {MODULE} is in PRODUCT_PACKAGES")


def mutations() -> list[tuple[str, str, str, str]]:
    """(label, source, text to find, text to put there): each one breaks one agreement."""
    package = "android.kernarium.sensoruse"
    return [
        ("overlay-string-other-class", FRAMEWORK_CONFIG, f"{package}.QuietActivity</string>",
         f"{package}.Other</string>"),
        ("overlay-string-stock", FRAMEWORK_CONFIG, f"{package}/{package}.QuietActivity</string>",
         "com.android.systemui/com.android.systemui.sensorprivacy.SensorUseStartedActivity</string>"),
        ("overlay-sets-software-dialog", FRAMEWORK_CONFIG, '<string name="config_sensorUseStartedActivity_hwToggle"',
         '<string name="config_sensorUseStartedActivity">x</string>\n    '
         '<string name="config_sensorUseStartedActivity_hwToggle"'),
        ("overlay-adds-software-toggle", FRAMEWORK_CONFIG, '<bool name="config_supportsHardwareCamToggle">true</bool>',
         '<bool name="config_supportsHardwareCamToggle">true</bool>\n    <bool name="config_supportsCamToggle">true</bool>'),
        ("overlay-drops-hardware-toggle", FRAMEWORK_CONFIG, '"config_supportsHardwareCamToggle">true<',
         '"config_supportsHardwareCamToggle">false<'),
        ("manifest-other-package", f"{MODULE_DIRECTORY}/AndroidManifest.xml", f'package="{package}"',
         'package="com.example.other"'),
        ("manifest-exported", f"{MODULE_DIRECTORY}/AndroidManifest.xml", 'android:exported="false"',
         'android:exported="true"'),
        ("manifest-theme", f"{MODULE_DIRECTORY}/AndroidManifest.xml", "@android:style/Theme.NoDisplay",
         "@android:style/Theme.Translucent"),
        ("manifest-not-direct-boot-aware", f"{MODULE_DIRECTORY}/AndroidManifest.xml",
         'android:directBootAware="true"', 'android:directBootAware="false"'),
        ("manifest-permission", f"{MODULE_DIRECTORY}/AndroidManifest.xml", "    <application",
         '    <uses-permission android:name="android.permission.CAMERA" />\n    <application'),
        ("class-does-not-finish", f"{MODULE_DIRECTORY}/src/android/kernarium/sensoruse/QuietActivity.java",
         "        finish();\n", "        setResult(RESULT_CANCELED);\n"),
        ("class-other-license", f"{MODULE_DIRECTORY}/src/android/kernarium/sensoruse/QuietActivity.java",
         "SPDX-License-Identifier: Apache-2.0", "SPDX-License-Identifier: GPL-2.0-or-later"),
        ("rule-not-product", f"{MODULE_DIRECTORY}/Android.bp", "product_specific: true,", "vendor: true,"),
        ("rule-other-name", f"{MODULE_DIRECTORY}/Android.bp", f'name: "{MODULE}"', 'name: "Other"'),
        ("product-drops-module", PRODUCT_MAKEFILE, f"PRODUCT_PACKAGES += {MODULE}\n", ""),
        ("product-adds-module-twice", PRODUCT_MAKEFILE, f"PRODUCT_PACKAGES += {MODULE}\n",
         f"PRODUCT_PACKAGES += {MODULE}\nPRODUCT_PACKAGES += {MODULE}\n"),
        ("build-script-drops-assertion", BUILD_SCRIPT, f'*" {MODULE} "*', '*" Other "*'),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    root = parser.parse_args().source_root.resolve()
    verifier = load_verifier(root)
    try:
        check(root, verifier)
    except ContractError as problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    failures = []
    with tempfile.TemporaryDirectory(prefix="sensor-use-contract-") as scratch:
        for label, source, old, new in mutations():
            copy = pathlib.Path(scratch) / label
            for relative in (*SOURCES, f"{MODULE_DIRECTORY}/src/android/kernarium/sensoruse/QuietActivity.java"):
                target = copy / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(root / relative, target)
            text = (copy / source).read_text()
            if text.count(old) != 1:
                failures.append(f"{label}: {old!r} occurs {text.count(old)} times in {source}")
                continue
            (copy / source).write_text(text.replace(old, new, 1))
            try:
                check(copy, verifier)
            except ContractError:
                continue
            failures.append(f"{label}: the check accepted the changed sources")
    if failures:
        print("FAIL:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print("sensor use contract: the sources agree and every change to one is caught")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
