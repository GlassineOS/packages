#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of scripts/changed.sh --arch on a made-up repository (a throwaway git
# repository in a temporary directory; no network).
#   scripts/test-changed.sh     exit 0 = all checks pass
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: got '$2', want '$3'"; fails=$((fails + 1)); fi; }
g() { git -C "$T" -c user.name=test -c user.email=test@invalid "$@"; }
ch() { sh "$T/scripts/changed.sh" "$@" 2>/dev/null | tr '\n' ' ' | sed 's/ $//'; }
recipe() { mkdir -p "$T/$1"; printf 'pkgname=%s\npkgver=1\narch="%s"\n' "${1#*/}" "$2" > "$T/$1/APKBUILD"; }

mkdir -p "$T/scripts" "$T/arches"
cp "$P/scripts/changed.sh" "$T/scripts/"
recipe keys/k noarch
recipe glassine/tool all
recipe glassine/panel all
recipe glassine/notx86 "all !x86_64"
recipe device/kern aarch64
printf '# comment\nkeys/k\nglassine/tool\nglassine/notx86\n' > "$T/arches/x86_64"
g init -q
g add -A
g commit -q -m base

check "all, no arch" "$(ch all)" "keys/k glassine/notx86 glassine/panel glassine/tool device/kern"
check "all, aarch64" "$(ch --arch aarch64 all)" "keys/k glassine/notx86 glassine/panel glassine/tool device/kern"
check "all, x86_64 (list and !x86_64)" "$(ch --arch x86_64 all)" "keys/k glassine/tool"

# only arch= changes: panel all -> aarch64, kern aarch64 -> "aarch64 x86_64"
sed -i 's/^arch=.*/arch="aarch64"/' "$T/glassine/panel/APKBUILD"
sed -i 's/^arch=.*/arch="aarch64 x86_64"/' "$T/device/kern/APKBUILD"
g commit -q -am arch
check "arch-only, no arch (as before)" "$(ch HEAD~1)" "glassine/panel device/kern"
check "arch-only, aarch64 already built" "$(ch --arch aarch64 HEAD~1)" ""
printf 'device/kern\n' >> "$T/arches/x86_64"
g commit -q -am list
check "arch-only, x86_64 newly allowed" "$(ch --arch x86_64 HEAD~2)" "device/kern"

# a real change beside arch=
sed -i 's/^pkgver=.*/pkgver=2/; s/^arch=.*/arch="all"/' "$T/glassine/panel/APKBUILD"
printf 'x\n' > "$T/glassine/tool/extra.patch"
g add -A
g commit -q -m real
check "version change rebuilds" "$(ch --arch aarch64 HEAD~1)" "glassine/panel glassine/tool"
check "x86_64 only the listed" "$(ch --arch x86_64 HEAD~1)" "glassine/tool"

[ "$fails" = 0 ] && echo "all checks pass" || { echo "$fails failed"; exit 1; }
