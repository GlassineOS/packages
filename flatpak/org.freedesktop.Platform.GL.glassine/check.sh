#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Check a built bundle the way a device uses it, in a privileged Alpine container with
# an empty system installation of its own:
#
#   flatpak/org.freedesktop.Platform.GL.glassine/check.sh BRANCH [OUT]
#
# OUT is build.sh's (default ./out). The checks:
#  1. the bundle installs system-wide with nothing else installed (no runtime, no
#     remote; a new device's first boot) and has the commit build.sh recorded;
#  2. pinned, it survives "flatpak uninstall --unused" (nothing refers to a GL
#     extension by name, so it counts as unused), and masked, "flatpak update" leaves
#     it alone;
#  3. with the runtime and Flathub's GL.default installed beside it and
#     FLATPAK_GL_DRIVERS=glassine:default, the sandbox's merged EGL vendor file and DRI
#     driver are ours.
set -eu
id=org.freedesktop.Platform.GL.glassine

if [ "${GLASSINE_IN_CONTAINER:-}" != 1 ]; then
	[ $# -ge 1 ] || { echo "usage: $0 BRANCH [OUT]" >&2; exit 2; }
	here=$(cd "$(dirname "$0")" && pwd)
	out=$(cd "${2:-$PWD/out}" && pwd)
	exec docker run --rm --privileged \
		-e GLASSINE_IN_CONTAINER=1 -e ARCH="${ARCH:-$(uname -m)}" \
		-v "$here":/recipe:ro -v "$out":/out:ro alpine:3.24 \
		sh "/recipe/check.sh" "$1"
fi

branch=$1
arch=$ARCH
[ "$arch" != arm64 ] || arch=aarch64
name=$id-$branch-$arch
ref=runtime/$id/$arch/$branch
gl=/usr/lib/$arch-linux-gnu/GL

apk add -q flatpak
export FLATPAK_SYSTEM_DIR=/tmp/system
mkdir -p "$FLATPAK_SYSTEM_DIR"
fail=0
no() { echo "FAIL: $*" >&2; fail=1; }

echo "1. install the bundle into an empty installation"
flatpak install --system -y --noninteractive --no-related --no-deps --bundle "/out/$name.flatpak"
want=$(cat "/out/$name.commit")
have=$(flatpak info --system -c "$ref")
[ "$have" = "$want" ] || no "installed commit $have, the bundle's is $want"

echo "2. pin and mask; uninstall --unused, update"
flatpak pin --system "$ref"
flatpak mask --system "$id//$branch"
flatpak uninstall --system -y --noninteractive --unused
flatpak info --system "$ref" >/dev/null 2>&1 || no "uninstall --unused removed the pinned extension"
flatpak update --system -y --noninteractive || no "flatpak update fails with the extension installed"
[ "$(flatpak info --system -c "$ref")" = "$want" ] || no "the commit changed after update"
flatpak install --system -y --noninteractive --reinstall --no-related --no-deps --bundle "/out/$name.flatpak" ||
	no "a masked extension doesn't reinstall from its bundle"

echo "3. beside the runtime and GL.default"
flatpak remote-add --system --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --system -y --noninteractive flathub "runtime/org.freedesktop.Platform/$arch/$branch" \
	"runtime/org.freedesktop.Platform.GL.default/$arch/$branch"
FLATPAK_GL_DRIVERS=glassine:default flatpak run --command=sh "org.freedesktop.Platform//$branch" -c "
	cat $gl/glvnd/egl_vendor.d/50_mesa.json
	readlink -f $gl/lib/dri/msm_dri.so
	ls $gl" > /tmp/sandbox.txt 2>&1 || { cat /tmp/sandbox.txt; no "flatpak run failed"; }
cat /tmp/sandbox.txt
grep -q "\"$gl/glassine/lib/libEGL_mesa.so.0\"" /tmp/sandbox.txt ||
	no "the merged 50_mesa.json isn't ours"
grep -qx "$gl/glassine/lib/dri/msm_dri.so\|$gl/glassine/lib/dri/[a-z_]*_dri.so\|$gl/glassine/lib/libgallium-.*-glassine.so" /tmp/sandbox.txt ||
	no "the merged lib/dri/msm_dri.so isn't ours"
[ $fail = 0 ] && echo "all checks passed for $name"
exit $fail
