#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of scripts/unpublished.sh on made-up recipes and indexes (a temporary
# directory; no network), and a parse check of every real recipe: each one's pkgname,
# pkgver and pkgrel must be plain values, or every publish would rebuild it.
#   scripts/test-unpublished.sh     exit 0 = all checks pass
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2', want '$3'"; fails=$((fails + 1)); fi; }
un() { sh "$T/scripts/unpublished.sh" "$@" 2>/dev/null | tr '\n' ' ' | sed 's/ $//'; }
recipe() { mkdir -p "$T/$1"; printf 'pkgname=%s\npkgver=%s\npkgrel=%s\narch="%s"\nsubpackages="$pkgname-doc"\n' "${1#*/}" "$2" "$3" "$4" > "$T/$1/APKBUILD"; }
# index: lines "name version"
index() { while read -r n v; do printf 'C:Q1x=\nP:%s\nV:%s\nA:%s\no:%s\n\n' "$n" "$v" "$1" "$n"; done; }

mkdir -p "$T/scripts" "$T/arches"
cp "$P/scripts/changed.sh" "$P/scripts/unpublished.sh" "$T/scripts/"
recipe keys/k 1 0 noarch
recipe glassine/tool 2.1 3 all
recipe glassine/panel 0_git20261121 1 aarch64
recipe forks/sway 1.10.1_git20261119 5 aarch64
recipe device/kern 7.3 0 aarch64
printf 'keys/k\nglassine/tool\n' > "$T/arches/x86_64"
(cd "$T" && git init -q && git -c user.name=t -c user.email=t@invalid add -A && git -c user.name=t -c user.email=t@invalid commit -q -m base)

index aarch64 > "$T/all" <<'X'
k 1-r0
tool 2.1-r3
tool-doc 2.1-r3
panel 0_git20261121-r1
sway 1.10.1_git20261119-r5
kern 7.3-r0
X
check "everything published" "$(un --arch aarch64 "$T/all")" ""

# the cancelled-push case: newer versions in the recipes than in the index, earlier
# versions of the same packages published
index aarch64 > "$T/old" <<'X'
k 1-r0
tool 2.1-r2
tool-doc 2.1-r3
panel 0_git20261121-r0
sway 1.10.1_git20261119-r4
sway 1.10.1_git20261115-r3
kern 7.3-r0
X
check "behind, in build order" "$(un --arch aarch64 "$T/old")" "glassine/panel glassine/tool forks/sway"
check "a subpackage at the version is not enough" "$(un --arch aarch64 "$T/old" | grep -c tool)" "1"
# an older version published beside the current one: published
index aarch64 > "$T/both" <<'X'
k 1-r0
tool 2.1-r2
tool 2.1-r3
panel 0_git20261121-r1
sway 1.10.1_git20261119-r5
kern 7.3-r0
kern 7.2-r9
X
check "earlier versions beside the current" "$(un --arch aarch64 "$T/both")" ""
# version matching is exact (no prefix match: r5 vs r50, 7.3 vs 7.3.1)
index aarch64 > "$T/near" <<'X'
k 1-r0
tool 2.1-r30
panel 0_git20261121-r1
sway 1.10.1_git20261119-r50
kern 7.3.1-r0
X
check "exact versions only" "$(un --arch aarch64 "$T/near")" "glassine/tool forks/sway device/kern"
# a new recipe, not published yet
recipe glassine/new 1 0 all
check "a new recipe" "$(un --arch aarch64 "$T/all")" "glassine/new"
rm -r "$T/glassine/new"
# x86_64: only the listed recipes, against its own index
printf 'k 1-r0\n' | index x86_64 > "$T/x86"
check "x86_64 list" "$(un --arch x86_64 "$T/x86")" "glassine/tool"
# empty index (nothing published under the name): all of the architecture's recipes
: > "$T/empty"
check "empty index" "$(un --arch aarch64 "$T/empty")" "keys/k glassine/panel glassine/tool forks/sway device/kern"
# not a plain value: built, with a warning
sed -i 's/^pkgrel=.*/pkgrel=$_rel/' "$T/device/kern/APKBUILD"
check "pkgrel not plain" "$(un --arch aarch64 "$T/all")" "device/kern"
check "pkgrel not plain warns" "$(sh "$T/scripts/unpublished.sh" --arch aarch64 "$T/all" 2>&1 >/dev/null | grep -c 'not a plain value')" "1"
sed -i 's/^pkgrel=.*/pkgrel="0"/' "$T/device/kern/APKBUILD"
check "quoted value" "$(un --arch aarch64 "$T/all")" ""
# runs from any directory, index path relative to it
check "relative index path" "$(cd "$T" && sh scripts/unpublished.sh --arch aarch64 all 2>/dev/null | tr '\n' ' ' | sed 's/ $//')" ""
check "unreadable index fails" "$(sh "$T/scripts/unpublished.sh" --arch aarch64 "$T/none" >/dev/null 2>&1 && echo 0 || echo 1)" "1"

# every real recipe has plain pkgname/pkgver/pkgrel
bad=
for f in "$P"/*/*/APKBUILD; do
	for k in pkgname pkgver pkgrel; do
		v=$(sed -n "s/^$k=//p" "$f" | tail -n 1)
		v=${v#\"} v=${v%\"}
		case "$v" in '' | *[!A-Za-z0-9._+-]*) bad="$bad ${f#"$P"/}:$k" ;; esac
	done
done
check "real recipes parse" "${bad# }" ""

[ "$fails" = 0 ] && echo "all checks pass" || { echo "$fails failed"; exit 1; }
