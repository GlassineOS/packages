#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Download a source archive and print its sha512sums line for an APKBUILD. Fails on
# any HTTP error (GitHub answers a rate limit with a short text body, which a plain
# download would happily checksum) and on an archive that does not unpack.
#   scripts/pin-source.sh URL [NAME]      NAME: the file name in source= (default: URL basename)
set -eu
url=$1
name=${2:-$(basename "$url")}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 5 --retry-delay 30 --retry-all-errors -o "$tmp/f" "$url"
case "$name" in
	*.tar.gz|*.tgz) gzip -t "$tmp/f"; tar -tzf "$tmp/f" >/dev/null ;;
	*.tar.xz) xz -t "$tmp/f"; tar -tJf "$tmp/f" >/dev/null ;;
esac
echo "$(stat -c %s "$tmp/f") bytes" >&2
echo "$(sha512sum < "$tmp/f" | cut -d' ' -f1)  $name"
