#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The accessibility artifact's builder refuses any toolchain but the one the
# tree pins, and a list without a Gradle distribution, before it builds
# anything; the packages it builds are the ones the stager requires and
# Andarium's ScreenReader recognises. It needs no prepared source, SDK or
# network: every refusal comes first.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
BUILDER="$ROOT_DIR/dependencies/build-andarium-accessibility.sh"
IDENTITY="$ROOT_DIR/dependencies/andarium-accessibility/identity.py"
SCREEN_READER="$ROOT_DIR/Andarium/AndariumCore/Sources/AndariumCore/ScreenReader.swift"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-andarium-accessibility.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM

TEST_NAME=test-build-andarium-accessibility.sh
. "$(dirname -- "$0")/../support/expect.sh"

"$BUILDER" --help | grep -F -- "--archive-manifest" >/dev/null || fail "--help lacks the preparation"

# A copy of the tracked inputs the builder reads, so a test can change one: the
# builder takes its root from its own location.
TREE="$TEST_ROOT/tree"
mkdir -p "$TREE/dependencies/andarium-accessibility"
cp "$BUILDER" "$TREE/dependencies/"
for file in identity.py toolchain.tsv archives.tsv; do
	cp "$ROOT_DIR/dependencies/andarium-accessibility/$file" "$TREE/dependencies/andarium-accessibility/"
done
COPY="$TREE/dependencies/build-andarium-accessibility.sh"
PINS="$ROOT_DIR/dependencies/andarium-accessibility/toolchain.tsv"
pinned()
{
	awk -F '\t' -v item="$1" '$1 == item { print $2 }' "$PINS"
}

# A stand-in JDK that reports a runtime version and an SDK whose components
# state their revisions, none of which runs in these checks.
make_jdk()
{
	mkdir -p "$1/bin"
	printf '#!/bin/sh\necho "    java.runtime.version = %s" >&2\n' "$2" >"$1/bin/java"
	chmod +x "$1/bin/java"
}
properties()
{
	mkdir -p "$(dirname -- "$1")"
	printf 'Pkg.Revision = %s\n' "$2" >"$1"
	[ -z "${3:-}" ] || printf 'AndroidVersion.ApiLevel = %s\n' "$3" >>"$1"
}
# make_sdk DIR NDK BUILD_TOOLS PLATFORM_API PLATFORM_REVISION CMAKE: each
# component sits in the directory the tree pins and states the given revision.
make_sdk()
{
	properties "$1/ndk/$(pinned ndk)/source.properties" "$2"
	properties "$1/build-tools/$(pinned build-tools)/source.properties" "$3"
	printf '#!/bin/sh\n' >"$1/build-tools/$(pinned build-tools)/aapt2"
	chmod +x "$1/build-tools/$(pinned build-tools)/aapt2"
	properties "$1/platforms/android-$(pinned platform-api)/source.properties" "$5" "$4"
	properties "$1/cmake/$(pinned cmake)/source.properties" "$6"
}
make_jdk "$TEST_ROOT/jdk" "$(pinned jdk)"
make_sdk "$TEST_ROOT/sdk" "$(pinned ndk)" "$(pinned build-tools)" "$(pinned platform-api)" \
	"$(pinned platform-revision)" "$(pinned cmake)"
build()
{
	env JAVA_HOME="$TEST_ROOT/jdk" ANDROID_HOME="$TEST_ROOT/sdk" "$COPY" "$@"
}

expect_diagnostic "unknown option" build --gradle x
expect_diagnostic "JAVA_HOME must name a JDK" \
	env JAVA_HOME="$TEST_ROOT/no-jdk" ANDROID_HOME="$TEST_ROOT/sdk" "$COPY"
expect_diagnostic "ANDROID_HOME must name an Android SDK" \
	env JAVA_HOME="$TEST_ROOT/jdk" ANDROID_HOME="$TEST_ROOT/no-sdk" "$COPY"
mkdir -p "$TEST_ROOT/empty-sdk"
expect_diagnostic "the SDK has no NDK $(pinned ndk)" \
	env JAVA_HOME="$TEST_ROOT/jdk" ANDROID_HOME="$TEST_ROOT/empty-sdk" "$COPY"
expect_diagnostic "the SDK has no build-tools $(pinned build-tools)" \
	env JAVA_HOME="$TEST_ROOT/jdk" ANDROID_HOME="$TEST_ROOT/empty-sdk" "$COPY"

# The pinned toolchain passes this stage (the next refusal is the unprepared
# sources), and each item swapped for another is refused by name, however
# its directory is called: the newest installed is never taken instead.
expect_diagnostic "the andarium-accessibility sources are not prepared" build
swap()
{
	label=$1
	shift
	make_sdk "$TEST_ROOT/swapped" "$@"
	expect_diagnostic "toolchain: $label" \
		env JAVA_HOME="$TEST_ROOT/jdk" ANDROID_HOME="$TEST_ROOT/swapped" "$COPY"
	rm -rf "$TEST_ROOT/swapped"
}
swap "ndk is 28.0.12433566, the tree pins $(pinned ndk)" 28.0.12433566 "$(pinned build-tools)" \
	"$(pinned platform-api)" "$(pinned platform-revision)" "$(pinned cmake)"
swap "build-tools is 36.2.0, the tree pins $(pinned build-tools)" "$(pinned ndk)" 36.2.0 \
	"$(pinned platform-api)" "$(pinned platform-revision)" "$(pinned cmake)"
swap "platform-api is 35, the tree pins $(pinned platform-api)" "$(pinned ndk)" "$(pinned build-tools)" \
	35 "$(pinned platform-revision)" "$(pinned cmake)"
swap "platform-revision is 9, the tree pins $(pinned platform-revision)" "$(pinned ndk)" \
	"$(pinned build-tools)" "$(pinned platform-api)" 9 "$(pinned cmake)"
swap "cmake is 3.31.6, the tree pins $(pinned cmake)" "$(pinned ndk)" "$(pinned build-tools)" \
	"$(pinned platform-api)" "$(pinned platform-revision)" 3.31.6
make_jdk "$TEST_ROOT/other-jdk" 21.0.9+10
expect_diagnostic "toolchain: jdk is 21.0.9+10, the tree pins $(pinned jdk)" \
	env JAVA_HOME="$TEST_ROOT/other-jdk" ANDROID_HOME="$TEST_ROOT/sdk" "$COPY"

# A Gradle distribution the builds need, removed from the list, is refused
# before anything is prepared.
for distribution in gradle-8.14.3-bin.zip gradle-9.6.1-bin.zip; do
	grep -v -F "$distribution" "$ROOT_DIR/dependencies/andarium-accessibility/archives.tsv" \
		>"$TREE/dependencies/andarium-accessibility/archives.tsv"
	expect_diagnostic "archives.tsv has no row for $distribution" build
done
cp "$ROOT_DIR/dependencies/andarium-accessibility/archives.tsv" "$TREE/dependencies/andarium-accessibility/"

# The packages the builder checks are those the stager requires, and
# TalkBack's is the one ScreenReader turns on and off.
for package in $(sed -n 's/^[A-Z_]*_PACKAGE=\(.*\)$/\1/p' "$BUILDER"); do
	grep -F "\"$package\"" "$IDENTITY" >/dev/null ||
		fail "the identity does not require $package, which the builder builds"
done
talkback=$(sed -n 's/^TALKBACK_PACKAGE=\(.*\)$/\1/p' "$BUILDER")
grep -F "\"$talkback/" "$SCREEN_READER" >/dev/null ||
	fail "ScreenReader does not recognise $talkback, which the builder builds"

printf '%s\n' "Andarium accessibility builder tests passed"
