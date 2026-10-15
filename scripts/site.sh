#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Write the site's landing page into the publish directory (Pages needs nothing else).
#   scripts/site.sh OUTDIR
set -eu
out=$1
cat > "$out/index.html" <<'HTML'
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GlassineOS packages</title>
<style>body{font:16px/1.5 system-ui,sans-serif;max-width:40rem;margin:2rem auto;padding:0 1rem}code,pre{background:#eee;padding:.1rem .3rem}</style>
<h1>GlassineOS packages</h1>
<p>Alpine package repository of <a href="https://github.com/GlassineOS">GlassineOS</a>.
GlassineOS images already use it. On another Alpine v3.22 system, put the
<a href="keys/">signing key</a> into <code>/etc/apk/keys/</code> and add this line to
<code>/etc/apk/repositories</code>:</p>
<pre>https://pkgs.glassine.org/alpine/v3.22/glassine</pre>
<p>After that, the <code>glassine-keys</code> package keeps both up to date.</p>
<p>Sources and build recipes: <a href="https://github.com/GlassineOS/packages">GlassineOS/packages</a>.</p>
HTML
mkdir -p "$out/keys"
cp keys/glassine-keys/*.rsa.pub "$out/keys/"
(cd "$out/keys" && for k in *.rsa.pub; do echo "<a href=\"$k\">$k</a><br>"; done) > "$out/keys/index.html"
