#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run scripts/build.sh in a clean Alpine container, natively (on an arm64 host the
# packages are aarch64). Same arguments as build.sh; ABUILD_PRIVKEY is passed through.
set -eu
cd "$(dirname "$0")/.."
exec docker run --rm -e ABUILD_PRIVKEY -v "$PWD:/src" alpine:3.22 sh /src/scripts/build.sh "$@"
