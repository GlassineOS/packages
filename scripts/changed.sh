#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Print the package directories to build, in build order (keys, glassine, forks,
# device: device packages depend on the others, nothing depends on them): those changed
# between BASE and HEAD, or all with "all" (or no usable BASE).
#   scripts/changed.sh BASE [HEAD]
set -eu
cd "$(dirname "$0")/.."
base=${1:-all}
head=${2:-HEAD}
if [ "$base" = all ] || ! git cat-file -e "$base^{commit}" 2>/dev/null; then
	dirs=$(for f in ./*/*/APKBUILD; do [ -f "$f" ] && dirname "${f#./}"; done)
else
	dirs=$(git diff --name-only "$base" "$head" -- keys device glassine forks | cut -d/ -f1,2 | sort -u)
fi
for top in keys glassine forks device; do
	for d in $dirs; do
		[ "${d%%/*}" = "$top" ] && [ -f "$d/APKBUILD" ] && echo "$d"
	done
done
exit 0
