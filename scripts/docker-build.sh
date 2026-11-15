#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Run scripts/build.sh in a clean Alpine container, natively (on an arm64 host the
# packages are aarch64, on an x86-64 host x86_64). Same arguments as build.sh;
# ABUILD_PRIVKEY and CAP (scripts/retain.sh) are passed through.
set -eu
cd "$(dirname "$0")/.."
exec docker run --rm -e ABUILD_PRIVKEY -e CAP -v "$PWD:/src" alpine:3.24 sh /src/scripts/build.sh "$@"
