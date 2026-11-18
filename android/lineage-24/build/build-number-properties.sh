# SPDX-License-Identifier: GPL-2.0-or-later
# Helpers sourced by build-aosp.sh, after common.sh, which supplies fail().
# They keep every generated property file of a build output naming the build
# number of the build, which Android's build does not do on its own: it reads
# the number without depending on it, so an incremental build over an earlier
# release's output keeps a property file none of whose other inputs changed.
# The step touches only the product's output directory: the product's installed
# tree without obj/, whose packaging intermediates the target-files rule
# rebuilds whole, and Soong's intermediates.

# The bound of one listing of the output directory, in seconds.
BUILD_PROPERTY_LIST_LIMIT=1800
# The listing's scratch file, inside the output directory; the name is fixed so
# that no run leaves a second one.
BUILD_PROPERTY_SCRATCH_NAME=.build-number-properties.list
BUILD_PROPERTY_SCRATCH=
BUILD_PROPERTY_COUNT=0
BUILD_PROPERTY_MISMATCHES=

# each_stale_build_property SCOPE OUT_DIR PRODUCT_DIR NUMBER COMMAND
#
# Run COMMAND with the path of every property file (build.prop or prop.default)
# whose ro.*build.version.incremental names a number other than NUMBER. SCOPE
# "all" lists the product's installed tree and Soong's intermediates;
# "installed" lists the installed tree alone. Both absolute paths name existing
# or absent directories, PRODUCT_DIR inside OUT_DIR. A missing directory has
# nothing to list. The listing is bounded, and a listing that fails or times
# out is an error: a partial scan would pass a stale file.
each_stale_build_property()
{
	local scope=$1 out_dir=$2 product_dir=$3 number=$4 visit=$5 path status
	local -a roots=()
	BUILD_PROPERTY_SCRATCH="$out_dir/$BUILD_PROPERTY_SCRATCH_NAME"
	case "$product_dir" in
	"$out_dir"/?*) ;;
	*) fail "the product output is not inside the output directory: $product_dir" ;;
	esac
	if [[ -d "$product_dir" ]]; then
		roots+=("$product_dir")
	fi
	if [[ "$scope" == all && -d "$out_dir/soong/.intermediates" ]]; then
		roots+=("$out_dir/soong/.intermediates")
	fi
	[[ ${#roots[@]} -gt 0 ]] || return 0
	if timeout -k 10 "$BUILD_PROPERTY_LIST_LIMIT" find "${roots[@]}" \
		-path "$product_dir/obj" -prune -o \
		-type f \( -name build.prop -o -name prop.default \) -print0 \
		>"$BUILD_PROPERTY_SCRATCH"; then
		status=0
	else
		status=$?
	fi
	if [[ $status -ne 0 ]]; then
		rm -f -- "$BUILD_PROPERTY_SCRATCH"
		fail "cannot list the property files under $out_dir (exit status $status)"
	fi
	while IFS= read -r -d '' path; do
		if awk -F= -v number="$number" \
			'$1 ~ /^ro\.([a-z0-9_]+\.)?build\.version\.incremental$/ && ($2 "") != (number "") { stale = 1 }
			END { exit !stale }' "$path" </dev/null; then
			"$visit" "$path"
		else
			status=$?
			if [[ $status -ne 1 ]]; then
				rm -f -- "$BUILD_PROPERTY_SCRATCH"
				fail "cannot read the property file $path (exit status $status)"
			fi
		fi
	done <"$BUILD_PROPERTY_SCRATCH"
	rm -f -- "$BUILD_PROPERTY_SCRATCH"
}

remove_stale_build_property()
{
	printf '%s\n' "removing a property file that names another build number: $1 ($(
		grep -E '^ro\.([a-z0-9_]+\.)?build\.version\.incremental=' "$1" </dev/null | tr '\n' ' '))"
	if ! rm -f -- "$1"; then
		rm -f -- "$BUILD_PROPERTY_SCRATCH"
		fail "cannot remove the property file that names another build number: $1"
	fi
	BUILD_PROPERTY_COUNT=$((BUILD_PROPERTY_COUNT + 1))
}

note_build_property_mismatch()
{
	BUILD_PROPERTY_MISMATCHES="$BUILD_PROPERTY_MISMATCHES
	$1"
}

# remove_stale_build_properties OUT_DIR PRODUCT_DIR NUMBER
#
# Before a build: remove, by the path found, every property file under the
# output directory that names another build number than NUMBER, so that the
# build makes it again. An absent output directory, or none stale, does nothing.
remove_stale_build_properties()
{
	local out_dir=$1 product_dir=$2 number=$3
	[[ -d "$out_dir" ]] || return 0
	BUILD_PROPERTY_COUNT=0
	each_stale_build_property all "$out_dir" "$product_dir" "$number" remove_stale_build_property
	if [[ $BUILD_PROPERTY_COUNT -gt 0 ]]; then
		printf '%s\n' "removed $BUILD_PROPERTY_COUNT property files that name another build number than $number"
	fi
}

# verify_build_number_properties OUT_DIR PRODUCT_DIR NUMBER
#
# After a build: refuse it if an installed property file names another build
# number than NUMBER.
verify_build_number_properties()
{
	local out_dir=$1 product_dir=$2 number=$3
	BUILD_PROPERTY_MISMATCHES=
	each_stale_build_property installed "$out_dir" "$product_dir" "$number" note_build_property_mismatch
	[[ -z "$BUILD_PROPERTY_MISMATCHES" ]] ||
		fail "the build left property files that name another build number than $number:$BUILD_PROPERTY_MISMATCHES"
}
