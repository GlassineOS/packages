#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Host test of scripts/retain.sh on made-up indexes (no network, no container).
#   scripts/test-retain.sh     exit 0 = all checks pass (CI runs it in alpine:3.24, as
#   the publish runs retain.sh there: busybox awk)
set -eu
P=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
D=86400 NOW=2000000000
export NOW
run() { sh "$P/scripts/retain.sh" "$T/live" "$T/built" "$T/locks" "$T/recipes" > "$T/out" 2> "$T/err"; }
has() { grep -qxF "$1" "$T/out"; }

# name version origin buildtime size
cat > "$T/live" <<EOF
shell 5 shell $((NOW - 2 * D)) 100
shell 4 shell $((NOW - 10 * D)) 100
shell 3 shell $((NOW - 40 * D)) 100
shell 2 shell $((NOW - 50 * D)) 100
shell-fonts 5 shell $((NOW - 2 * D)) 10
kernel 1 kernel $((NOW - 90 * D)) 1000
model 2 model $((NOW - 5 * D)) 50
gone 1 gone $((NOW - 1 * D)) 10
gone-locked 1 gone-locked $((NOW - 1 * D)) 10
EOF
printf 'kernel 2 1000\nmodel 2 50\n' > "$T/built"
printf 'shell=2\ngone-locked=1\nalpine-baselayout=3.7.2-r1\n' > "$T/locks"
printf 'shell\nkernel\nmodel\n' > "$T/recipes"
run
check "the newest version stays" 'has "shell 5" && has "shell-fonts 5"'
check "a version replaced 2 days ago stays" 'has "shell 4"'
check "a version replaced 10 days ago stays" 'has "shell 3"'
check "the version this run replaces stays, whatever its age" 'has "kernel 1"'
check "a rebuilt version is not taken from the repository" '! has "model 2"'
check "a package whose recipe is gone goes" '! has "gone 1" && grep -q "drop gone 1 (recipe gone gone)" "$T/err"'
check "... unless a lock names it" 'has "gone-locked 1"'
# shell 2: replaced by 3, built 40 days ago, but locked
check "a locked version stays past the window" 'has "shell 2"'

printf '' > "$T/locks"
run
check "without the lock, the version replaced 40 days ago goes" '! has "shell 2" && grep -q "drop shell 2 (replaced 40 days ago)" "$T/err"'
DAYS=45 run
check "DAYS sets the window" 'has "shell 2"'

# size cap: built 1050; kept newest shell 5 (100) + fonts (10) + kernel 1 (replaced now, 1000)
# + shell 4 (replaced 2 days ago) + shell 3 (replaced 10 days ago) = 2360
printf 'shell=3\n' > "$T/locks"
CAP=2300 run
check "over the cap, the longest-replaced unlocked version goes first" '! has "shell 4" && grep -q "drop shell 4 (size cap" "$T/err"'
check "... and only as many as needed" 'has "kernel 1" && has "shell 3" && has "shell 5"'
check "locked and newest alone over the cap: fails" '! CAP=1200 run && grep -q "more than 1200" "$T/err"'

: > "$T/live"; : > "$T/built"
run
check "an empty repository keeps nothing" '[ ! -s "$T/out" ]'

[ "$fails" = 0 ] && echo "all checks pass" || { echo "$fails failed"; exit 1; }
