#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build packages with abuild into one repository directory. Runs as root in a clean
# alpine container with this repository at /src (scripts/docker-build.sh starts it):
#
#   scripts/build.sh [--merge URL] DIR...      e.g. device/linux-glassine-sdm660
#
# Output: out/alpine/<branch>/glassine/<arch>/ with *.apk and a signed APKINDEX.
# Signing: ABUILD_PRIVKEY (PEM) must match one of keys/glassine-keys/*.rsa.pub; without
# it an ephemeral key signs (a check build, never published).
# --merge URL: also take every package of the published repository at URL whose
# recipe still exists and that was not rebuilt now (the publish job; Pages replaces
# the whole site, so each deploy must carry everything).
set -eu

BRANCH=v3.22
merge=
if [ "${1:-}" = --merge ]; then merge=$2; shift 2; fi
[ $# -gt 0 ] || { echo "usage: $0 [--merge URL] DIR..." >&2; exit 2; }

cd /src
arch=$(apk --print-arch)
out=/src/out/alpine/$BRANCH/glassine/$arch

apk add -q alpine-sdk openssl
id builder >/dev/null 2>&1 || { adduser -D builder; addgroup builder abuild; }
home=/home/builder
mkdir -p "$home/.abuild" "$home/keys"

# signing key
if [ -n "${ABUILD_PRIVKEY:-}" ]; then
	printf '%s\n' "$ABUILD_PRIVKEY" > "$home/priv.pem"
	pub=$(openssl rsa -in "$home/priv.pem" -pubout 2>/dev/null)
	name=
	for k in keys/glassine-keys/*.rsa.pub; do
		[ "$(cat "$k")" = "$pub" ] && name=$(basename "$k" .pub)
	done
	[ -n "$name" ] || { echo "ABUILD_PRIVKEY matches no key in keys/glassine-keys" >&2; exit 1; }
	mv "$home/priv.pem" "$home/.abuild/$name"
	cp "keys/glassine-keys/$name.pub" "$home/.abuild/"
	echo "PACKAGER_PRIVKEY=$home/.abuild/$name" > "$home/.abuild/abuild.conf"
	cp keys/glassine-keys/*.rsa.pub "$home/keys/"
else
	echo "no ABUILD_PRIVKEY: ephemeral key, check build only"
	chown -R builder "$home"
	su builder -c 'abuild-keygen -a -n -q' >/dev/null
	name=$(basename "$(sed -n 's/^PACKAGER_PRIVKEY="\{0,1\}\([^"]*\)"\{0,1\}$/\1/p' "$home/.abuild/abuild.conf")")
	cp "$home/.abuild/$name.pub" "$home/keys/"
fi
# abuild -r installs packages built earlier in this run as makedepends
cp "$home"/keys/*.pub /etc/apk/keys/
echo "PACKAGER=\"GlassineOS CI <4208996+Biont@users.noreply.github.com>\"" >> "$home/.abuild/abuild.conf"

# build in a copy owned by the build user (the checkout belongs to the runner)
rm -rf "$home/work" "$home/repo"
mkdir -p "$home/work" "$home/repo"
for d in "$@"; do
	[ -f "$d/APKBUILD" ] || { echo "$d: no APKBUILD" >&2; exit 1; }
	mkdir -p "$home/work/$d"
	cp -a "$d/." "$home/work/$d/"
done
chown -R builder "$home"
for d in "$@"; do
	grep -qxF "$home/repo/${d%%/*}" /etc/apk/repositories || echo "$home/repo/${d%%/*}" >> /etc/apk/repositories
done
for d in "$@"; do
	echo "::group::abuild $d"
	su builder -c "cd '$home/work/$d' && REPODEST='$home/repo' abuild -r"
	echo "::endgroup::"
done

# one flat repository: abuild sorts by the recipe's parent directory, we don't
rm -rf "$out"
mkdir -p "$out"
find "$home/repo" -name '*.apk' -path "*/$arch/*" -exec cp {} "$out/" \;
built=$(cd "$out" && for f in *.apk; do tar -xzOf "$f" .PKGINFO 2>/dev/null | sed -n 's/^pkgname = //p'; done)

if [ -n "$merge" ]; then
	tmp=$home/merge
	mkdir -p "$tmp"
	if wget -q -O "$tmp/APKINDEX.tar.gz" "$merge/APKINDEX.tar.gz"; then
		apk --keys-dir "$home/keys" verify "$tmp/APKINDEX.tar.gz"
		tar -xzOf "$tmp/APKINDEX.tar.gz" APKINDEX | awk -v RS= '{
			p = v = o = ""
			n = split($0, l, "\n")
			for (i = 1; i <= n; i++) {
				if (l[i] ~ /^P:/) p = substr(l[i], 3)
				if (l[i] ~ /^V:/) v = substr(l[i], 3)
				if (l[i] ~ /^o:/) o = substr(l[i], 3)
			}
			print p, v, o
		}' > "$tmp/list"
		while read -r p v o; do
			echo "$built" | grep -qxF "$p" && continue
			ls -d ./*/"$o"/APKBUILD >/dev/null 2>&1 || { echo "drop $p (recipe $o gone)"; continue; }
			wget -q -O "$out/$p-$v.apk" "$merge/$p-$v.apk"
			apk --keys-dir "$home/keys" verify "$out/$p-$v.apk" >/dev/null
		done < "$tmp/list"
	else
		echo "nothing published at $merge yet"
	fi
fi

# index over everything, signed with the same key
rm -f "$out/APKINDEX.tar.gz"
chown -R builder "$out"
su builder -c "cd '$out' && apk index -q -o APKINDEX.tar.gz --rewrite-arch $arch \
	--description 'GlassineOS $BRANCH' *.apk && abuild-sign -q APKINDEX.tar.gz"
chown -R "$(stat -c %u /src):$(stat -c %g /src)" /src/out
ls -la "$out"
