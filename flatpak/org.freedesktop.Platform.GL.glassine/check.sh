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
#  3. with Flathub's GL.default masked for the branch (as mesa-flatpak-glassine-sync
#     does), installing the runtime with its related refs and updating doesn't bring
#     GL.default, and in the sandbox EGL gives a GLES2 context from our Mesa (llvmpipe
#     here: no GPU) and Vulkan lists lavapipe (probe.py), with libgallium, libLLVM and
#     libvulkan_lvp loaded from our extension;
#  4. with GL.default unmasked and installed beside it and
#     FLATPAK_GL_DRIVERS=glassine:default, the sandbox's merged EGL vendor file, DRI
#     driver and lavapipe ICD file are ours, and the probe still loads our libraries.
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
flatpak info --system "$ref" | grep -i "installed"

echo "2. pin and mask; uninstall --unused, update"
flatpak pin --system "$ref"
flatpak mask --system "$id//$branch"
flatpak uninstall --system -y --noninteractive --unused
flatpak info --system "$ref" >/dev/null 2>&1 || no "uninstall --unused removed the pinned extension"
flatpak update --system -y --noninteractive || no "flatpak update fails with the extension installed"
[ "$(flatpak info --system -c "$ref")" = "$want" ] || no "the commit changed after update"
flatpak install --system -y --noninteractive --reinstall --no-related --no-deps --bundle "/out/$name.flatpak" ||
	no "a masked extension doesn't reinstall from its bundle"

# probe.py in the sandbox, with only GL extensions named in FLATPAK_GL_DRIVERS enabled;
# the output in /tmp/probe-$1.txt. The runtime's Vulkan loader finds each ICD file twice
# (GL/vulkan/icd.d and /usr/share/vulkan/icd.d, a link to it), so lavapipe is listed twice.
probe() {
	FLATPAK_GL_DRIVERS=glassine:default flatpak run --command=python3 "org.freedesktop.Platform//$branch" - \
		< /recipe/probe.py > "/tmp/probe-$1.txt" 2>&1 || no "probe.py failed in the sandbox ($1)"
	cat "/tmp/probe-$1.txt"
	grep -Eq '^gl: renderer (FD[0-9]|llvmpipe)' "/tmp/probe-$1.txt" || no "no GLES2 context from freedreno or llvmpipe ($1)"
	grep -q '^vulkan: device llvmpipe' "/tmp/probe-$1.txt" || no "Vulkan doesn't list lavapipe ($1)"
	for so in 'libgallium-[^/]*-glassine\.so' 'libLLVM[^/]*\.so[^/]*' 'libvulkan_lvp\.so'; do
		grep -Eq "^loaded: $gl/glassine/lib/$so\$" "/tmp/probe-$1.txt" || no "$so isn't loaded from our extension ($1)"
	done
	# beside GL.default, the Vulkan loader also loads its other ICDs and layers; its Mesa
	# (libgallium, libEGL_mesa) must not load
	if [ "$1" = absent ]; then
		! grep -q "^loaded: $gl/default/" "/tmp/probe-$1.txt" || no "GL.default's libraries are loaded ($1)"
	else
		! grep -Eq "^loaded: $gl/default/lib/lib(gallium|EGL_mesa)" "/tmp/probe-$1.txt" || no "GL.default's Mesa is loaded ($1)"
	fi
}
default=org.freedesktop.Platform.GL.default

echo "3. without GL.default (masked for the branch)"
flatpak mask --system "$default//$branch"
flatpak mask --system "$default//$branch-extra"
flatpak remote-add --system --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --system -y --noninteractive flathub "runtime/org.freedesktop.Platform/$arch/$branch"
flatpak update --system -y --noninteractive || no "flatpak update fails with GL.default masked"
for b in "$branch" "$branch-extra"; do
	! flatpak info --system "runtime/$default/$arch/$b" >/dev/null 2>&1 || no "$default//$b got installed although masked"
done
flatpak list --system --columns=ref
probe absent

echo "4. beside GL.default"
flatpak mask --system --remove "$default//$branch"
flatpak mask --system --remove "$default//$branch-extra"
flatpak install --system -y --noninteractive flathub "runtime/$default/$arch/$branch"
FLATPAK_GL_DRIVERS=glassine:default flatpak run --command=sh "org.freedesktop.Platform//$branch" -c "
	cat $gl/glvnd/egl_vendor.d/50_mesa.json; echo
	cat $gl/vulkan/icd.d/lvp_icd.$arch.json; echo
	readlink -f $gl/lib/dri/msm_dri.so
	ls $gl" > /tmp/sandbox.txt 2>&1 || { cat /tmp/sandbox.txt; no "flatpak run failed"; }
cat /tmp/sandbox.txt
grep -q "\"$gl/glassine/lib/libEGL_mesa.so.0\"" /tmp/sandbox.txt ||
	no "the merged 50_mesa.json isn't ours"
grep -q "\"$gl/glassine/lib/libvulkan_lvp.so\"" /tmp/sandbox.txt ||
	no "the merged lvp_icd.$arch.json isn't ours"
grep -qx "$gl/glassine/lib/dri/msm_dri.so\|$gl/glassine/lib/dri/[a-z_]*_dri.so\|$gl/glassine/lib/libgallium-.*-glassine.so" /tmp/sandbox.txt ||
	no "the merged lib/dri/msm_dri.so isn't ours"
probe beside
[ $fail = 0 ] && echo "all checks passed for $name"
exit $fail
