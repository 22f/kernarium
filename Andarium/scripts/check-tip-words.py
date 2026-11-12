#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the words of Buy me tokens against App Store Review Guideline 3.1.1.

A tip is a voluntary one-time purchase, so no string of the tip jar's table
names a donation, a plan or a subscription, promises an unlock, an upgrade or
premium access, or quotes a token amount or balance, in any language the table
has: the English words, and the Chinese and Japanese ones that say the same.
The table also says plainly, in its English source, that a tip is a voluntary
one-time purchase and that every feature is free either way.

The table is a String Catalog (`Localization/TipJar.xcstrings`) or a compiled
strings file (`TipJar.strings` of an application's language directory); every
key and every localized value is read, and a comment, which no one sees, is not.
Run it on the catalog, and again, once the translations are in, on the
application's tables.

    check-tip-words.py Andarium/Localization/TipJar.xcstrings
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import plistlib
import re
import sys
from pathlib import Path

# The words each language's table must not use, as regular expressions: English
# whole words and their forms, and the Chinese (Simplified and Traditional) and
# Japanese words for the same ideas.
FORBIDDEN = (
    ("donate", re.compile(r"\bdonat\w*|\bdonor\w*", re.IGNORECASE)),
    ("plan", re.compile(r"\bplans?\b", re.IGNORECASE)),
    ("subscription", re.compile(r"\bsubscri\w*", re.IGNORECASE)),
    ("unlock", re.compile(r"\bunlock\w*", re.IGNORECASE)),
    ("premium", re.compile(r"\bpremium\b", re.IGNORECASE)),
    ("upgrade", re.compile(r"\bupgrad\w*", re.IGNORECASE)),
    ("credits", re.compile(r"\bcredits?\b", re.IGNORECASE)),
    ("balance", re.compile(r"\bbalances?\b", re.IGNORECASE)),
    ("a donation", re.compile("捐赠|捐贈|捐款|捐助|捐献|捐獻|斗内|斗內|寄付|寄附|募金")),
    ("a subscription", re.compile("订阅|訂閱|購読|定期購入|サブスク")),
    ("a plan", re.compile("方案|计划|計劃|計畫|プラン")),
    ("an unlock", re.compile("解锁|解鎖|アンロック|ロック解除")),
    ("premium or upgrade", re.compile("高级版|高級版|プレミアム|升级|升級|アップグレード")),
    ("credits", re.compile("积分|積分|点数|點數|クレジット")),
    ("a balance", re.compile("余额|余額|餘額|残高")),
)
# A number, or a placeholder for one, beside the word token.
AMOUNT = re.compile(
    r"(?:\d|%(?:\d+\$)?(?:l{0,2}d|f|@))[\d.,]*\s*[kKmM]?\s*(?:tokens?|トークン)"
    r"|(?:tokens?|トークン)\s*[:：]?\s*(?:\d|%(?:\d+\$)?(?:l{0,2}d|f|@))",
    re.IGNORECASE)
# What the English source must say, each word in one string: voluntary, one-time,
# a purchase, and free either way.
REQUIRED = ("voluntary", "one-time", "purchase", "free")


def texts_of_catalog(data: dict) -> list[tuple[str, str, str]]:
    """(key, language, text) for every key of a String Catalog (the key itself is
    the source language's text) and every value localized for it."""
    source = data.get("sourceLanguage", "en")
    found = []
    for key, entry in data.get("strings", {}).items():
        found.append((key, source, key))
        for language, localization in (entry.get("localizations") or {}).items():
            units = []
            if "stringUnit" in localization:
                units.append(localization["stringUnit"])
            for variation in (localization.get("variations") or {}).values():
                units.extend(item.get("stringUnit", {}) for item in variation.values())
            found.extend((key, language, unit["value"]) for unit in units if isinstance(unit.get("value"), str))
    return found


def texts_of_strings(path: Path) -> list[tuple[str, str, str]]:
    """(key, language, text) for a compiled strings file, the language being its
    directory's."""
    language = path.parent.name.removesuffix(".lproj")
    raw = path.read_bytes()
    try:
        table = plistlib.loads(raw)
    except (plistlib.InvalidFileException, ValueError):
        # An old-style text file: UTF-16 with a mark, or UTF-8.
        text = raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8", errors="replace")
        return [(text[:40], language, text)]
    found = []
    for key, value in table.items():
        found.append((str(key), language, str(key)))
        found.extend((str(key), language, text) for text in flatten(value))
    return found


def flatten(value: object) -> list[str]:
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        return [text for item in value.values() for text in flatten(item)]
    if isinstance(value, (list, tuple)):
        return [text for item in value for text in flatten(item)]
    return []


def problems_in(texts: list[tuple[str, str, str]], name: str) -> list[str]:
    found = []
    seen = set()
    for key, language, text in texts:
        if (key, language, text) in seen:
            continue
        seen.add((key, language, text))
        for label, pattern in FORBIDDEN:
            match = pattern.search(text)
            if match:
                found.append(f"{name}: {key!r} [{language}] uses {label} ({match.group(0)!r})")
        match = AMOUNT.search(text)
        if match:
            found.append(f"{name}: {key!r} [{language}] quotes a token amount ({match.group(0)!r})")
    return found


def check(path: Path) -> tuple[list[str], int]:
    """The problems of one table and how many texts it has."""
    if path.suffix == ".xcstrings":
        data = json.loads(path.read_text(encoding="utf-8"))
        texts = texts_of_catalog(data)
        source = data.get("sourceLanguage", "en")
        found = problems_in(texts, path.name)
        english = [text for key, language, text in texts if language == source == "en"]
        for word in REQUIRED:
            if not any(word in text.lower() for text in english):
                found.append(f"{path.name}: no English string says {word!r}: a tip is a voluntary one-time "
                             "purchase, and every feature is free either way")
        return found, len(texts)
    texts = texts_of_strings(path)
    return problems_in(texts, str(path)), len(texts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tables", nargs="+", type=Path, help="a TipJar.xcstrings or a TipJar.strings")
    options = parser.parse_args()
    found: list[str] = []
    count = 0
    for path in options.tables:
        try:
            problems, texts = check(path)
        except (OSError, ValueError) as error:
            found.append(f"{path}: cannot be read: {error}")
            continue
        found.extend(problems)
        count += texts
    for line in found:
        print(line, file=sys.stderr)
    if found:
        return 1
    print(f"{len(options.tables)} table(s), {count} texts: no forbidden word, no token amount")
    return 0


if __name__ == "__main__":
    sys.exit(main())
