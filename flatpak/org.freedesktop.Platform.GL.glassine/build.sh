#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the Flatpak GL extension org.freedesktop.Platform.GL.glassine for one runtime
# branch, in a privileged Alpine container (bwrap needs real namespaces):
#
#   flatpak/org.freedesktop.Platform.GL.glassine/build.sh BRANCH [OUT]
#
# BRANCH is the GL extension version of the apps' runtime: 25.08 for
# org.freedesktop.Platform//25.08, 26.08 for org.gnome.Platform//51 and
# org.freedesktop.Platform//26.08 (the runtime's [Extension org.freedesktop.Platform.GL]
# "versions"). The architecture is the host's (ARCH overrides it); aarch64 is the one
# devices use, and builds natively on an arm64 host. OUT (default ./out) gets
#
#   org.freedesktop.Platform.GL.glassine-BRANCH-ARCH.flatpak   the bundle
#   org.freedesktop.Platform.GL.glassine-BRANCH-ARCH.commit    its OSTree commit
#   org.freedesktop.Platform.GL.glassine-BRANCH-ARCH.log       flatpak-builder's log
#
# and keeps the Sdk and flatpak-builder's state in OUT/cache (several GB; reused).
# SOURCE_DATE_EPOCH, when set, is the commit's timestamp.
set -eu
id=org.freedesktop.Platform.GL.glassine

if [ "${GLASSINE_IN_CONTAINER:-}" != 1 ]; then
	[ $# -ge 1 ] || { echo "usage: $0 BRANCH [OUT]" >&2; exit 2; }
	here=$(cd "$(dirname "$0")" && pwd)
	root=$(cd "$here/../.." && pwd)
	mkdir -p "${2:-$PWD/out}"
	out=$(cd "${2:-$PWD/out}" && pwd)
	exec docker run --rm --privileged \
		-e GLASSINE_IN_CONTAINER=1 -e ARCH="${ARCH:-$(uname -m)}" -e HOST_UID="$(id -u)" \
		-e SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-}" \
		-v "$root":/src:ro -v "$out":/out alpine:3.24 \
		sh "/src/flatpak/$id/build.sh" "$1"
fi

# passed through empty when the caller has none; flatpak-builder rejects an empty one
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
branch=$1
case $branch in
[0-9][0-9].[0-9][0-9]) ;;
*) echo "branch: e.g. 25.08, not $branch" >&2; exit 2 ;;
esac
arch=$ARCH
case $arch in
aarch64 | x86_64) ;;
arm64) arch=aarch64 ;;
*) echo "arch: aarch64 or x86_64, not $arch" >&2; exit 2 ;;
esac
name=$id-$branch-$arch
cache=/out/cache/$branch-$arch
log=/out/$name.log
prefix=/usr/lib/$arch-linux-gnu/GL/glassine

apk add -q flatpak flatpak-builder ostree binutils
# what this run writes belongs to the caller, also after a failure
trap 'chown -R "$HOST_UID" /out' EXIT
# root in the container: a system installation of its own (flatpak refuses a user one)
export FLATPAK_SYSTEM_DIR=$cache/flatpak
mkdir -p "$FLATPAK_SYSTEM_DIR"
flatpak --system remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# Mesa's version, checksum and patches: those of the system Mesa (forks/mesa)
apkbuild=/src/forks/mesa/APKBUILD
mesa=$(sed -n 's/^pkgver=//p' "$apkbuild")
sha512=$(awk -v f="mesa-$mesa.tar.xz" '$2 == f { print $1 }' "$apkbuild")
[ -n "$mesa" ] && [ ${#sha512} = 128 ] || { echo "no Mesa version or checksum in $apkbuild" >&2; exit 1; }

# the manifest, with the patches copied next to it (flatpak-builder takes sources
# relative to the manifest)
w=$cache/work
rm -rf "$w/tree" "$w/build"
mkdir -p "$w/tree/patches"
patches=
for p in /src/forks/mesa/glassine-*.patch; do
	[ -f "$p" ] || { echo "no glassine-*.patch in /src/forks/mesa" >&2; exit 1; }
	cp "$p" "$w/tree/patches/"
	patches="$patches      - type: patch
        path: patches/${p##*/}
"
done
printf '%s' "$patches" > "$w/patches.yml"
sed -e "s|@BRANCH@|$branch|g" -e "s|@ARCH@|$arch|g" -e "s|@MESA_VERSION@|$mesa|g" \
	-e "s|@MESA_SHA512@|$sha512|g" "/src/flatpak/$id/$id.yml.in" |
	awk -v pf="$w/patches.yml" '$0 == "@PATCHES@" { while ((getline l < pf) > 0) print l; next } { print }' \
	> "$w/tree/$id.yml"
! grep -n '@[A-Z][A-Z0-9_]*@' "$w/tree/$id.yml" || { echo "unfilled fields in the manifest" >&2; exit 1; }

echo "building $id//$branch ($arch): Mesa $mesa, $(ls "$w/tree/patches" | wc -l) patches"
cd "$w"
if ! flatpak-builder --arch="$arch" --system --install-deps-from=flathub --force-clean \
	--disable-rofiles-fuse --state-dir="$w/state" "$w/build" "$w/tree/$id.yml" >"$log" 2>&1; then
	grep -E "^(error|Error|FAILED|meson.build:)" -A12 "$log" | head -80
	tail -30 "$log"
	exit 1
fi

# Which GL extension wins is its [ExtensionOf] priority (higher first; equal ones in
# name order, so "default" would come before "glassine"), not the order of
# FLATPAK_GL_DRIVERS. The first one supplies every merged file (egl_vendor.d, lib/dri,
# lib/gbm) and comes first in the library path. Above default's 0, ours comes first.
md=$w/build/metadata
grep -q '^\[ExtensionOf\]' "$md" || { echo "no [ExtensionOf] in the metadata" >&2; cat "$md" >&2; exit 1; }
awk '{ print } /^\[ExtensionOf\]/ { print "priority=10" }' "$md" > "$md.new" && mv "$md.new" "$md"

# what makes our Mesa the one that loads
f=$w/build/files
lib=libgallium-$mesa-glassine.so
fail=0
[ -f "$f/lib/$lib" ] || { echo "missing: lib/$lib" >&2; fail=1; }
for so in lib/libEGL_mesa.so.0 lib/libGLX_mesa.so.0 lib/gbm/dri_gbm.so; do
	readelf -d "$f/$so" 2>/dev/null | grep -q "(NEEDED).*\[$lib\]" || { echo "$so doesn't link $lib" >&2; fail=1; }
done
for j in glvnd/egl_vendor.d/50_mesa.json share/glvnd/egl_vendor.d/50_mesa.json; do
	grep -q "\"$prefix/lib/libEGL_mesa.so.0\"" "$f/$j" || { echo "$j doesn't name $prefix/lib/libEGL_mesa.so.0" >&2; fail=1; }
done
ls "$f/lib/dri/" | grep -qx msm_dri.so || { echo "missing: lib/dri/msm_dri.so" >&2; fail=1; }
[ $fail = 0 ] || exit 1
cat "$md"

ts=NOW
[ -z "${SOURCE_DATE_EPOCH:-}" ] || ts=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
rm -rf "$w/repo"
flatpak build-export --runtime --files=files --arch="$arch" --timestamp="$ts" \
	--subject="Mesa $mesa with the GlassineOS freedreno patches" "$w/repo" "$w/build" "$branch" >>"$log" 2>&1
flatpak build-bundle --runtime --arch="$arch" "$w/repo" "/out/$name.flatpak" "$id" "$branch"
ostree --repo="$w/repo" rev-parse "runtime/$id/$arch/$branch" > "/out/$name.commit"
echo "$name: commit $(cat "/out/$name.commit"), $(du -h "/out/$name.flatpak" | cut -f1)"
sha256sum "/out/$name.flatpak"
