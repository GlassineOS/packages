#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Which packages of the published repository the next publish keeps beside the ones it
# builds now (scripts/build.sh --merge). The repository keeps earlier versions, so an
# image whose lock names one (GlassineOS/images, stages/*/lock) still builds after a
# newer version is published; only a lock refresh moves the lock.
#
#   scripts/retain.sh LIVE BUILT LOCKS RECIPES
#
# LIVE     the published index, one line per package: name version origin buildtime size
# BUILT    the packages built in this run: name version size
# LOCKS    lock lines name=version (others are ignored; the file may be empty)
# RECIPES  the origins whose recipe still exists, one per line
# Env: NOW (seconds, default now), DAYS (default 30), CAP (bytes, default 900 MiB).
#
# Prints "name version" for each published package to keep. Rules, per name:
# - a version rebuilt now replaces the published one of the same version;
# - the newest version stays;
# - an older version stays while the version that replaced it is younger than DAYS days
#   (so every version stays at least DAYS days after it stops being the newest);
# - a version a lock names stays, whatever its age;
# - a package whose recipe is gone goes, unless a lock names it;
# - when everything together is larger than CAP, the replaced versions no lock names go,
#   the longest-replaced first, until it fits; if the locked and newest versions alone
#   are larger than CAP, it fails (GitHub Pages serves at most 1 GB).
# Notes on what it drops go to stderr.
set -eu
[ $# = 4 ] || { echo "usage: $0 LIVE BUILT LOCKS RECIPES" >&2; exit 2; }
NOW=${NOW:-$(date +%s)} DAYS=${DAYS:-30} CAP=${CAP:-943718400}
awk -v now="$NOW" -v days="$DAYS" -v cap="$CAP" '
FILENAME == ARGV[1] { live[++nl] = $0; next }
FILENAME == ARGV[2] { b[$1 " " $2] = 1; bt[$1] = 1; total += $3; next }
FILENAME == ARGV[3] { if (index($0, "=")) { i = index($0, "="); lock[substr($0, 1, i - 1) " " substr($0, i + 1)] = 1 } next }
FILENAME == ARGV[4] { recipe[$1] = 1; next }
END {
	# candidates: published versions not rebuilt now, grouped by name
	for (i = 1; i <= nl; i++) {
		split(live[i], f, " ")
		k = f[1] " " f[2]
		if (k in b) continue
		n = ++cnt[f[1]]
		ver[f[1], n] = f[2]; org[f[1], n] = f[3]; bts[f[1], n] = f[4] + 0; sz[f[1], n] = f[5] + 0
	}
	keepn = 0
	for (p in cnt) {
		m = cnt[p]
		# newest first by build time (a version built now is newer than all of them)
		for (i = 1; i <= m; i++) ord[i] = i
		for (i = 2; i <= m; i++) for (j = i; j > 1 && bts[p, ord[j]] > bts[p, ord[j - 1]]; j--) {
			x = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = x
		}
		for (r = 1; r <= m; r++) {
			i = ord[r]; v = ver[p, i]; k = p " " v
			locked = (k in lock)
			if (r == 1 && !(p in bt)) { replaced = -1 }	# still the newest
			else if (r == 1) { replaced = now }		# replaced by this run
			else { replaced = bts[p, ord[r - 1]] }
			if (!(org[p, i] in recipe) && !locked) { print "drop " k " (recipe " org[p, i] " gone)" > "/dev/stderr"; continue }
			if (replaced >= 0 && !locked && now - replaced >= days * 86400) {
				print "drop " k " (replaced " int((now - replaced) / 86400) " days ago)" > "/dev/stderr"; continue
			}
			keepn++; kk[keepn] = k; ks[keepn] = sz[p, i]; total += sz[p, i]
			# only replaced, unlocked versions may go for the size cap
			kr[keepn] = (replaced >= 0 && !locked) ? replaced : -1
		}
	}
	while (total > cap) {
		o = 0
		for (i = 1; i <= keepn; i++) if (kr[i] >= 0 && (o == 0 || kr[i] < kr[o])) o = i
		if (o == 0) {
			printf "retain.sh: the newest and the locked versions alone take %d bytes, more than %d\n", total, cap > "/dev/stderr"
			exit 1
		}
		print "drop " kk[o] " (size cap " cap " bytes)" > "/dev/stderr"
		total -= ks[o]; kr[o] = -2; kk[o] = ""
	}
	for (i = 1; i <= keepn; i++) if (kk[i] != "") print kk[i]
	printf "retain.sh: %d published packages kept, %d bytes with this run'"'"'s\n", keepn, total > "/dev/stderr"
}' "$1" "$2" "$3" "$4"
