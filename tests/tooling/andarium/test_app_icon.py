#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that each configuration of Andarium compiles its own app icon.

APP_ICON_NAME (Config/Andarium.xcconfig, Andarium/README.md#configurations) is
DuskJade in every configuration but TestFlight, which has DuskEmber; the target
compiles it (ASSETCATALOG_COMPILER_APPICON_NAME), leaves the other document out
of the build and includes no other app icon asset. The AndariumPreview target
keeps DuskJade throughout. The bundle check sees the built bundle's icon; this
binds the settings that produce it, which xcodebuild resolves for each
configuration.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import subprocess
import sys
from pathlib import Path

PROJECT = Path("Andarium/Andarium.xcodeproj")
ICON_JADE = "DuskJade"
ICON_EMBER = "DuskEmber"

# Each configuration of the target and the app icon it compiles.
CONFIGURATIONS = (
    ("Debug", ICON_JADE),
    ("Release", ICON_JADE),
    ("TestFlight", ICON_EMBER),
    ("AppStore", ICON_JADE),
)


def run(command: list[str], timeout: int = 90) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=timeout)


def target_settings(root: Path, configuration: str) -> dict[str, dict[str, str]]:
    """The resolved build settings of every target in a configuration, by target."""
    done = run(["xcrun", "xcodebuild", "-project", str(root / PROJECT), "-alltargets",
                "-configuration", configuration, "-showBuildSettings", "-json"])
    if done.returncode != 0:
        raise SystemExit(f"{configuration}: xcodebuild -showBuildSettings failed:\n{done.stderr}")
    return {item["target"]: item["buildSettings"] for item in json.loads(done.stdout)}


def icon_problems(configuration: str, target: str, resolved: dict[str, str], icon: str) -> list[str]:
    """What differs in a target's resolved settings from compiling `icon` alone."""
    other = ICON_EMBER if icon == ICON_JADE else ICON_JADE
    where = f"{configuration} {target}"
    excluded = resolved.get("EXCLUDED_SOURCE_FILE_NAMES", "").split()
    found = []
    for setting in ("APP_ICON_NAME", "ASSETCATALOG_COMPILER_APPICON_NAME"):
        if resolved.get(setting) != icon:
            found.append(f"{where}: {setting} is {resolved.get(setting)}, not {icon}")
    if f"{other}.icon" not in excluded or f"{icon}.icon" in excluded:
        found.append(f"{where}: does not leave {other}.icon alone out of the build")
    if resolved.get("ASSETCATALOG_COMPILER_INCLUDE_ALL_APPICON_ASSETS") != "NO":
        found.append(f"{where}: includes app icon assets besides {icon}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    root = parser.parse_args().root
    failures: list[str] = []
    for configuration, icon in CONFIGURATIONS:
        targets = target_settings(root, configuration)
        failures += icon_problems(configuration, "Andarium", targets["Andarium"], icon)
        failures += icon_problems(configuration, "AndariumPreview", targets["AndariumPreview"], ICON_JADE)
    for failure in failures:
        print(failure, file=sys.stderr)
    if failures:
        return 1
    print("every configuration compiles its own app icon")
    return 0


if __name__ == "__main__":
    sys.exit(main())
