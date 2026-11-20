#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The archive switch of the AOT component gates the accepted artifact.

cmake/UmlAotArchives.cmake reads LINEAGE24_ACCEPTED_AOT_ARCHIVES from the
accepted AOT descriptor. The rows run its function with `cmake -P` on synthetic
descriptors, then check that darwin/cpu/CMakeLists.txt reads it for the accepted
artifact: an iOS runtime takes the artifact only when the descriptor says yes,
and an image blob is never an archive input.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ARGUMENTS: argparse.Namespace


def cmake(script: str, **definitions: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "probe.cmake"
        path.write_text(
            f'include("{ARGUMENTS.module}")\n{script}\n', encoding="utf-8")
        options = [f"-D{name}={value}" for name, value in definitions.items()]
        return subprocess.run(
            [ARGUMENTS.cmake, *options, "-P", str(path)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60, check=False)


class ArchiveSwitchTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = Path(self.enterContext(tempfile.TemporaryDirectory()))

    def descriptor(self, text: str | None) -> Path:
        path = self.directory / "accepted-aot.env"
        path.unlink(missing_ok=True)
        if text is not None:
            path.write_text(text, encoding="utf-8")
        return path

    def state(self, descriptor: Path) -> subprocess.CompletedProcess:
        return cmake('uml_aot_archives_state(state "${DESCRIPTOR}")\n'
                     'message("state=${state}")', DESCRIPTOR=str(descriptor))

    def test_the_state_is_what_the_descriptor_says(self) -> None:
        for text, expected in (
            ("LINEAGE24_ACCEPTED_AOT_ARCHIVES='yes'\n", "yes"),
            ("A='1'\nLINEAGE24_ACCEPTED_AOT_ARCHIVES='no'\nB='2'\n", "no"),
            (None, "none"),
        ):
            with self.subTest(expected=expected):
                result = self.state(self.descriptor(text))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"state={expected}", result.stderr)

    def test_a_descriptor_that_states_no_switch_is_refused(self) -> None:
        for text in ("A='1'\n", "LINEAGE24_ACCEPTED_AOT_ARCHIVES='maybe'\n",
                     "LINEAGE24_ACCEPTED_AOT_ARCHIVES=yes\n"):
            with self.subTest(text=text):
                result = self.state(self.descriptor(text))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("LINEAGE24_ACCEPTED_AOT_ARCHIVES of yes or no", result.stderr)


class WiringTests(unittest.TestCase):
    """darwin/cpu/CMakeLists.txt takes the component under the switch."""

    def setUp(self) -> None:
        self.text = Path(ARGUMENTS.cpu_lists).read_text(encoding="utf-8")

    def test_the_accepted_artifact_reads_the_same_switch(self) -> None:
        self.assertIn("uml_aot_archives_state(_uml_aot_archives", self.text)
        self.assertNotIn("LINEAGE24_ACCEPTED_AOT_ARCHIVES", self.text.replace("(LINEAGE24_ACCEPTED_AOT_ARCHIVES)", ""))

    def test_an_image_blob_is_not_an_archive_input(self) -> None:
        # UML_AOT_IMAGE qualifies a release on macOS and cannot be an iOS product.
        image = re.search(r"if\(UML_AOT_IMAGE\)(.*?)else\(\)", self.text, re.S)
        self.assertIsNotNone(image)
        self.assertIn("NOT UML_TARGET_MACOS", image.group(1))


def main() -> int:
    global ARGUMENTS
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--cpu-lists", required=True)
    ARGUMENTS, rest = parser.parse_known_args()
    result = unittest.main(argv=[sys.argv[0], *rest], exit=False).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
