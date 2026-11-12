#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what the Buy me tokens word check refuses and what it accepts."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

STATEMENT = ("A tip is a voluntary one-time purchase that supports Andarium’s independent development. "
             "Every feature is free either way.")


def catalog(strings: dict[str, dict[str, str]]) -> str:
    """A String Catalog: each key with the values localized for it, by language."""
    return json.dumps({
        "sourceLanguage": "en", "version": "1.0",
        "strings": {key: {"comment": "A note for translators: plan, donate, subscription.",
                          "localizations": {language: {"stringUnit": {"state": "translated", "value": value}}
                                            for language, value in values.items()}}
                    for key, values in strings.items()},
    }, ensure_ascii=False, indent=2)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--table", type=Path, required=True, help="the application's TipJar.xcstrings")
    options = parser.parse_args()
    failures: list[str] = []

    def run(*paths: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(options.checker), *map(str, paths)], check=False, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=60)

    def expect(label: str, result: subprocess.CompletedProcess[str], *, passes: bool, said: str = "") -> None:
        if (result.returncode == 0) != passes:
            failures.append(f"{label}: expected {'a pass' if passes else 'a failure'}:\n{result.stdout}")
        elif said and said not in result.stdout:
            failures.append(f"{label}: output lacks {said!r}:\n{result.stdout}")

    with tempfile.TemporaryDirectory(prefix="andarium-tip-words-") as directory:
        root = Path(directory)

        def table(name: str, strings: dict[str, dict[str, str]], *, statement: bool = True) -> Path:
            path = root / name / "TipJar.xcstrings"
            path.parent.mkdir()
            everything = {STATEMENT: {}, **strings} if statement else strings
            path.write_text(catalog(everything), encoding="utf-8")
            return path

        # The application's own table passes.
        expect("the table", run(options.table), passes=True, said="no forbidden word")

        # Plain words pass, including the ones that only contain a forbidden word.
        expect("clean", run(table("clean", {
            "Buy me tokens": {"ja": "トークンをおごる", "zh-Hans": "请我吃点Token"},
            "Please explain the planet, planting and the planar plane.": {},
            "Tips aren’t available right now": {},
            "Pro, $4.99": {},
            "Token": {},
        })), passes=True)

        # Each English word, in a key and in a localized value, in any case.
        for index, word in enumerate((
                "donate", "Donation", "DONATIONS", "donor", "plan", "Plans", "subscription", "Subscribe", "subscriber",
                "unlock", "Unlocked", "premium", "Upgrade", "credit", "Credits", "balance")):
            expect(f"key-{word}", run(table(f"key-{index}", {f"Please {word} now": {}})), passes=False, said="uses")
            expect(f"value-{word}", run(table(f"value-{index}", {"Thanks": {"en": f"Thanks, {word}!"}})),
                   passes=False, said="uses")

        # The Chinese and Japanese words for the same ideas, in the language that has them.
        for index, (language, text) in enumerate((
                ("zh-Hans", "感谢您的捐赠"), ("zh-Hant", "感謝您的捐款"),
                ("zh-Hant", "斗內"), ("zh-Hans", "订阅更多"), ("zh-Hant", "訂閱"),
                ("zh-Hans", "选择方案"), ("zh-Hans", "升级高级版"),
                ("zh-Hans", "解锁功能"), ("zh-Hant", "余額不足"),
                ("ja", "寄付をお願いします"), ("ja", "サブスクリプション"),
                ("ja", "プランを選ぶ"), ("ja", "プレミアム機能"),
                ("ja", "アップグレード"), ("ja", "残高"))):
            expect(f"{language}-{index}", run(table(f"cjk-{index}", {"Thanks": {language: text}})),
                   passes=False, said=f"[{language}]")

        # A token amount or balance, in a number or a placeholder, either side of the word.
        for index, text in enumerate((
                "You have 500 tokens", "You have 1,000 Tokens", "500k tokens", "2.5M tokens", "%lld tokens",
                "%d token", "%1$lld tokens", "Tokens: 12", "tokens 5", "%@ tokens", "トークン 500",
                "12トークン")):
            expect(f"amount-{index}", run(table(f"amount-{index}", {text: {}})), passes=False, said="token amount")

        # The statement is required, in the English source: each of its ideas.
        expect("no-statement", run(table("no-statement", {"Buy me tokens": {}}, statement=False)),
               passes=False, said="'voluntary'")
        for index, word in enumerate(("voluntary", "one-time", "purchase", "free")):
            expect(f"missing-{word}", run(table(f"missing-{index}", {
                STATEMENT.lower().replace(word, "x"): {}}, statement=False)), passes=False, said=f"{word!r}")

        # A compiled strings table of a language: the same words, by the directory's language.
        compiled = root / "ja.lproj" / "TipJar.strings"
        compiled.parent.mkdir()
        compiled.write_bytes(plistlib.dumps({"Done": "寄付"}, fmt=plistlib.FMT_BINARY))
        expect("compiled-bad", run(compiled), passes=False, said="[ja]")
        compiled.write_bytes(plistlib.dumps({"Done": "完了", "Try again": "もう一度"}, fmt=plistlib.FMT_BINARY))
        expect("compiled-ok", run(compiled), passes=True)
        compiled.write_bytes('"Done" = "Please subscribe";\n'.encode())
        expect("compiled-text", run(compiled), passes=False, said="uses")
        expect("both", run(options.table, compiled), passes=False, said="uses")

        # An unreadable table is no pass.
        broken = root / "broken" / "TipJar.xcstrings"
        broken.parent.mkdir()
        broken.write_text("{", encoding="utf-8")
        expect("broken", run(broken), passes=False, said="cannot be read")
        expect("absent", run(root / "absent" / "TipJar.xcstrings"), passes=False, said="cannot be read")

    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
