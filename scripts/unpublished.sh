#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Print the recipes published for ARCH whose current version the published index lacks,
# in build order (scripts/changed.sh --arch ARCH all): a recipe is published when its
# main package (pkgname) is in INDEX at pkgver-rpkgrel. The publish workflow builds these
# beside the recipes its push changed, so a recipe change whose own publish run never ran
# (a newer push replaced its pending run in the concurrency group, a run failed or was
# cancelled) is built by the next run. A recipe whose pkgname, pkgver or pkgrel is not a
# plain value is printed too (with a warning): building it is the safe side.
#   scripts/unpublished.sh --arch ARCH INDEX
# INDEX: the published APKINDEX (the text file inside APKINDEX.tar.gz).
set -eu
[ $# = 3 ] && [ "$1" = --arch ] || { echo "usage: $0 --arch ARCH INDEX" >&2; exit 2; }
arch=$2 index=$3
[ -r "$index" ] || { echo "$0: $index: not readable" >&2; exit 1; }
case "$index" in /*) ;; *) index=$PWD/$index ;; esac
cd "$(dirname "$0")/.."
# the last plain assignment of a variable in an APKBUILD (empty: none, or not plain)
val() {
	v=$(sed -n "s/^$1=//p" "$2" | tail -n 1)
	v=${v#\"} v=${v%\"}
	case "$v" in *[!A-Za-z0-9._+-]*) v= ;; esac
	printf '%s' "$v"
}
have=$(awk '/^P:/ { p = substr($0, 3) } /^V:/ { print p "=" substr($0, 3) }' "$index")
for d in $(scripts/changed.sh --arch "$arch" all); do
	n=$(val pkgname "$d/APKBUILD") v=$(val pkgver "$d/APKBUILD") r=$(val pkgrel "$d/APKBUILD")
	if [ -z "$n" ] || [ -z "$v" ] || [ -z "$r" ]; then
		echo "$d: pkgname, pkgver or pkgrel is not a plain value: building it" >&2
		echo "$d"
	elif ! printf '%s\n' "$have" | grep -qxF "$n=$v-r$r"; then
		echo "$d"
	fi
done
