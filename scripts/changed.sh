#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Print the package directories to build, in build order (keys, glassine, forks,
# device: device packages depend on the others, nothing depends on them): those changed
# between BASE and HEAD, or all with "all" (or no usable BASE).
#   scripts/changed.sh [--arch ARCH] BASE [HEAD]
# --arch ARCH: only the recipes published for ARCH: the recipe's arch= allows it (all,
# noarch or ARCH, and no !ARCH), and, if arches/ARCH exists, the recipe is listed there.
# With --arch, a change to a recipe that touches only its arch= line builds nothing for
# an ARCH it already built for (the published packages stay as they are); it builds
# for an ARCH it adds.
set -eu
cd "$(dirname "$0")/.."
arch=
if [ "${1:-}" = --arch ]; then arch=$2; shift 2; fi
base=${1:-all}
head=${2:-HEAD}

# allows ARCH: the arch= value of an APKBUILD (on stdin) admits $arch
allows() {
	a=$(sed -n 's/^arch="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' | tail -n 1)
	ok=
	for t in $a; do
		case "$t" in
		all | noarch | "$arch") ok=1 ;;
		"!$arch") return 1 ;;
		esac
	done
	[ -n "$ok" ]
}
listed() {
	[ -f "arches/$arch" ] || return 0
	grep -v '^#' "arches/$arch" | grep -qxF "$1"
}

all=
if [ "$base" = all ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
	all=1
	dirs=$(for f in ./*/*/APKBUILD; do [ -f "$f" ] && dirname "${f#./}"; done)
else
	dirs=$(git diff --name-only "$base" "$head" -- keys device glassine forks | cut -d/ -f1,2 | sort -u)
fi
for top in keys glassine forks device; do
	for d in $dirs; do
		[ "${d%%/*}" = "$top" ] && [ -f "$d/APKBUILD" ] || continue
		if [ -n "$arch" ]; then
			listed "$d" && allows < "$d/APKBUILD" || continue
			# only arch= changed, and the recipe built for $arch before: nothing new
			if [ -z "$all" ] &&
				[ "$(git diff --name-only "$base" "$head" -- "$d")" = "$d/APKBUILD" ] &&
				git show "$base:$d/APKBUILD" 2>/dev/null | allows &&
				[ "$(git show "$base:$d/APKBUILD" | grep -v '^arch=')" = \
					"$(git show "$head:$d/APKBUILD" | grep -v '^arch=')" ]; then
				echo "$d: only arch= changed, $arch keeps its published packages" >&2
				continue
			fi
		fi
		echo "$d"
	done
done
exit 0
