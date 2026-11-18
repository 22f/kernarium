#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the accessibility artifact the LineageOS product carries: TalkBack and
# eSpeak NG as release APKs, unsigned (the Android release signs them), and
# artifact.json naming them and the source trees they came from
# (android/lineage-24/README.md#accessibility).
#
# The build is offline and consumes only what dependencies/prepare.sh fetched
# and verified for the andarium-accessibility product: the TalkBack, eSpeak NG
# and libsonic sources, and the archives of
# dependencies/andarium-accessibility/archives.tsv (the Gradle distributions
# and every Maven artifact). Gradle runs with --offline, a Gradle user home of
# its own and a Maven repository laid out from those archives, to which an
# init script points every repository the builds name. The JDK and the SDK
# components are those dependencies/andarium-accessibility/toolchain.tsv
# names, each read from its own metadata (identity.py), and artifact.json
# records them with the pins, series trees, archive list and this script's
# identity, which the stager checks against the tree.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SOURCE_ROOT=${UML_DEPENDENCY_SOURCE_ROOT:-"$ROOT_DIR/external/sources"}
CACHE_ROOT=${UML_DEPENDENCY_CACHE:-"$ROOT_DIR/.build/dependency-cache"}
ARCHIVES="$ROOT_DIR/dependencies/andarium-accessibility/archives.tsv"
OUTPUT="$ROOT_DIR/.build/andarium-accessibility"
WORK_ROOT="$ROOT_DIR/.build/dependency-build/andarium-accessibility"
TALKBACK_GRADLE=gradle-8.14.3
ESPEAK_GRADLE=gradle-9.6.1
IDENTITY="$ROOT_DIR/dependencies/andarium-accessibility/identity.py"
# The packages Andarium's ScreenReader recognises and the voice it speaks with.
TALKBACK_PACKAGE=com.android.talkback
SPEECH_PACKAGE=com.reecedunn.espeak

fail()
{
	printf '%s\n' "dependencies/build-andarium-accessibility.sh: $*" >&2
	exit 1
}

usage()
{
	cat <<'EOF'
Usage: dependencies/build-andarium-accessibility.sh [--output PATH]

Builds TalkBack and eSpeak NG offline from the prepared andarium-accessibility
sources and archives, and writes the artifact (TalkBack.apk, eSpeakNG.apk,
artifact.json) to .build/andarium-accessibility or PATH.

Prepare its inputs first:

  dependencies/prepare.sh --product andarium-accessibility
  dependencies/prepare.sh --archive-manifest dependencies/andarium-accessibility/archives.tsv

Requires JAVA_HOME and ANDROID_HOME naming the JDK and the Android SDK whose
versions dependencies/andarium-accessibility/toolchain.tsv pins, on an Apple
silicon Mac, the host the pinned AAPT2 runs on. Any other version is refused.
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--output)
		[ "$#" -ge 2 ] || fail "--output requires a path"
		OUTPUT=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*) fail "unknown option: $1" ;;
	esac
done

[ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/java" ] || fail "JAVA_HOME must name a JDK"
[ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME" ] || fail "ANDROID_HOME must name an Android SDK"
python3 -B "$IDENTITY" archives --require "$TALKBACK_GRADLE-bin.zip" --require "$ESPEAK_GRADLE-bin.zip" ||
	fail "the archive list lacks a Gradle distribution the builds need"
python3 -B "$IDENTITY" toolchain --java-home "$JAVA_HOME" --android-home "$ANDROID_HOME" ||
	fail "the JDK and SDK are not the toolchain dependencies/andarium-accessibility/toolchain.tsv pins"
BUILD_TOOLS="$ANDROID_HOME/build-tools/$(python3 -B "$IDENTITY" tracked build-tools)"

"$ROOT_DIR/dependencies/prepare.sh" --product andarium-accessibility --verify >/dev/null ||
	fail "the andarium-accessibility sources are not prepared; run dependencies/prepare.sh --product andarium-accessibility"
"$ROOT_DIR/dependencies/prepare.sh" --archive-manifest "$ARCHIVES" --verify >/dev/null ||
	fail "the andarium-accessibility archives are not prepared; run dependencies/prepare.sh --archive-manifest $ARCHIVES"

rm -rf "$WORK_ROOT"
mkdir -p "$WORK_ROOT"
python3 -B "$IDENTITY" toolchain --java-home "$JAVA_HOME" --android-home "$ANDROID_HOME" \
	--output "$WORK_ROOT/toolchain.json"

# The Maven repository and the Gradle distributions, from the verified cache.
# Each module's maven-metadata.xml lists the versions pinned, so a dynamic
# version such as 1.0.+ resolves to the pinned one.
python3 -B - "$ARCHIVES" "$CACHE_ROOT/archives" "$WORK_ROOT" <<'PY_LAYOUT'
import collections
import csv
import pathlib
import sys
import zipfile
from xml.sax.saxutils import escape

archives, cache, work = (pathlib.Path(argument) for argument in sys.argv[1:])
with archives.open() as stream:
    rows = list(csv.reader((line for line in stream if not line.startswith("#")), delimiter="\t"))
versions = collections.defaultdict(set)
for name, url, digest in rows[1:]:
    cached = cache / f"{digest}-{name}"
    if name.startswith("maven~"):
        group, module, version, file = name.split("~")[1:]
        target = work / "maven" / group.replace(".", "/") / module / version / file
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(cached)
        versions[(group, module)].add(version)
    elif name.endswith("-bin.zip"):
        with zipfile.ZipFile(cached) as distribution:
            distribution.extractall(work)
        for script in (work / name[: -len("-bin.zip")] / "bin").iterdir():
            script.chmod(0o755)
    else:
        raise SystemExit(f"unknown archive {name}")
for (group, module), pinned in versions.items():
    listed = "".join(f"      <version>{escape(version)}</version>\n" for version in sorted(pinned))
    (work / "maven" / group.replace(".", "/") / module / "maven-metadata.xml").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<metadata>\n'
        f"  <groupId>{escape(group)}</groupId>\n  <artifactId>{escape(module)}</artifactId>\n"
        f"  <versioning>\n    <versions>\n{listed}    </versions>\n  </versioning>\n</metadata>\n",
        encoding="utf-8",
    )
PY_LAYOUT

# Every Maven repository a build or its settings names becomes the local one,
# and plugins resolve from it too, so nothing is looked up elsewhere. A
# buildscript's __plugin_repository__ entries are Gradle's views of the
# plugin repositories, which are already local. Gradle pins a component's
# .module but not the .pom that points to it, so the .module is read first.
cat >"$WORK_ROOT/offline.gradle" <<EOF
def local = new File('$WORK_ROOT/maven').toURI()
def redirect = { repositories ->
    repositories.all { repository ->
        if (repository.name.startsWith('__plugin_repository__')) {
            return
        }
        if (!(repository instanceof MavenArtifactRepository)) {
            throw new GradleException("offline build: \${repository.name} is not a Maven repository")
        }
        repository.url = local
        repository.metadataSources { gradleMetadata(); mavenPom() }
    }
}
beforeSettings { settings ->
    settings.pluginManagement.repositories.maven { url = local }
    redirect(settings.pluginManagement.repositories)
    redirect(settings.dependencyResolutionManagement.repositories)
}
allprojects { project ->
    redirect(project.buildscript.repositories)
    redirect(project.repositories)
}
EOF

# Each source is built from a clean export of its prepared commit, so the
# prepared checkout stays as prepare.sh verifies it.
for source in talkback espeak-ng sonic; do
	mkdir -p "$WORK_ROOT/$source"
	git -C "$SOURCE_ROOT/$source" archive HEAD | tar -x -C "$WORK_ROOT/$source"
done
printf 'sdk.dir=%s\n' "$ANDROID_HOME" >"$WORK_ROOT/talkback/local.properties"
printf 'sdk.dir=%s\n' "$ANDROID_HOME" >"$WORK_ROOT/espeak-ng/android/local.properties"

gradle()
{
	distribution=$1
	project=$2
	shift 2
	"$WORK_ROOT/$distribution/bin/gradle" --offline --no-daemon --console=plain \
		--gradle-user-home "$WORK_ROOT/gradle-home" --init-script "$WORK_ROOT/offline.gradle" \
		--project-dir "$project" "$@"
}

gradle "$ESPEAK_GRADLE" "$WORK_ROOT/espeak-ng/android" -PsonicSourceDir="$WORK_ROOT/sonic" assembleRelease
gradle "$TALKBACK_GRADLE" "$WORK_ROOT/talkback" assemblePhoneRelease

rm -rf "$OUTPUT.partial"
mkdir -p "$OUTPUT.partial"

# Copies $1 to $2 after checking that it is the package $3.
stage_apk()
{
	[ -f "$1" ] || fail "$1 was not built"
	package=$("$BUILD_TOOLS/aapt2" dump packagename "$1")
	[ "$package" = "$3" ] || fail "$2 is package $package, not $3"
	cp "$1" "$OUTPUT.partial/$2"
}

stage_apk "$WORK_ROOT/espeak-ng/android/build/outputs/apk/release/espeak-release-unsigned.apk" \
	eSpeakNG.apk "$SPEECH_PACKAGE"
stage_apk "$WORK_ROOT/talkback/build/outputs/apk/phone/release/talkback-phone-release-unsigned.apk" \
	TalkBack.apk "$TALKBACK_PACKAGE"

# No APK may hold a value of this machine: its checkout, its work root, its
# home directory or its user name.
set -- --forbid "$ROOT_DIR" --forbid "$WORK_ROOT"
[ "${#HOME}" -gt 3 ] && set -- "$@" --forbid "$HOME"
python3 -B "$IDENTITY" scan "$@" "$OUTPUT.partial/TalkBack.apk" "$OUTPUT.partial/eSpeakNG.apk" ||
	fail "an APK holds a value of this machine"

python3 -B "$IDENTITY" describe --sources "$SOURCE_ROOT" --apks "$OUTPUT.partial" \
	--toolchain "$WORK_ROOT/toolchain.json" --output "$OUTPUT.partial/artifact.json"

rm -rf "$OUTPUT"
mv "$OUTPUT.partial" "$OUTPUT"
printf '%s\n' "wrote the accessibility artifact to $OUTPUT"
