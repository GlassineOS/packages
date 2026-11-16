#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The GlassineOS default splash for a grey panel, as a P5 picture (8-bit grey).

    splash.py WIDTH HEIGHT OUT.pgm

White, with the GlassineOS mark in its middle: the mark of the documentation site
(GlassineOS/docs, overrides/partials/logo.html, a 48 x 48 view box): a sheet's outline,
a second sheet turned 6 degrees over it, a pen stroke on that sheet. Drawn here from the
same shapes, in greys, with each pixel's coverage from the shapes' signed distances. Only
+, -, *, / and sqrt are used, so the bytes are the same on every machine.
"""
import sys

# the mark's colours (the site's white voice: ink, highlight, pen) as panel greys
BG, INK, LEAF, PEN = 255, 26, 208, 26
# cos and sin of 6 degrees
C6, S6 = 0.9945218953682733, 0.10452846326765347


def clamp(x):
    return 0.0 if x < 0.0 else 1.0 if x > 1.0 else x


def box(px, py, cx, cy, hw, hh, r):
    """Signed distance from (px, py) to a rounded rectangle (centre, half sizes, radius)."""
    qx, qy = abs(px - cx) - hw + r, abs(py - cy) - hh + r
    ox, oy = max(qx, 0.0), max(qy, 0.0)
    return (ox * ox + oy * oy) ** 0.5 + min(max(qx, qy), 0.0) - r


def bezier(p0, p1, p2, p3, n):
    pts = []
    for i in range(n + 1):
        t = i / n
        u = 1.0 - t
        a, b, c, d = u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t
        pts.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return pts


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy))
    ex, ey = px - ax - t * dx, py - ay - t * dy
    return (ex * ex + ey * ey) ** 0.5


def main():
    w, h, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    # the mark: 48 units, ~41 % of the narrow side, its centre a little above the middle
    unit = min(w, h) * 0.41 / 48.0
    ox, oy = (w - 48 * unit) / 2.0, h * 0.44 - 24 * unit
    stroke = bezier((20, 30), (26, 24), (30, 34), (38, 26), 96)
    img = bytearray([BG]) * (w * h)
    x0, x1 = int(ox) - 2, int(ox + 50 * unit) + 2
    y0, y1 = int(oy) - 2, int(oy + 50 * unit) + 2
    half = 0.5 / unit          # half a pixel, in view-box units
    for y in range(max(y0, 0), min(y1, h)):
        vy = (y + 0.5 - oy) / unit
        row = y * w
        for x in range(max(x0, 0), min(x1, w)):
            vx = (x + 0.5 - ox) / unit
            v = float(BG)
            # 1. the sheet: rect x=2 y=4 30x34 rx 5, outline 2.5 wide
            d = abs(box(vx, vy, 17.0, 21.0, 15.0, 17.0, 5.0)) - 1.25
            a = clamp(0.5 - d / (2 * half))
            v += (INK - v) * a
            # 2. the leaf: rect x=14 y=10 30x34 rx 5, turned 6 degrees about (29, 27)
            rx, ry = vx - 29.0, vy - 27.0
            lx, ly = C6 * rx + S6 * ry + 29.0, -S6 * rx + C6 * ry + 27.0
            d = box(lx, ly, 29.0, 27.0, 15.0, 17.0, 5.0)
            a = clamp(0.5 - d / (2 * half))
            if a > 0.0:
                v += (LEAF - v) * a
                # 3. the pen stroke on the leaf, 3 wide, round ends
                if 18.0 < lx < 40.0 and 22.0 < ly < 35.0:
                    d = min(seg_dist(lx, ly, *stroke[i], *stroke[i + 1]) for i in range(len(stroke) - 1)) - 1.5
                    a = clamp(0.5 - d / (2 * half))
                    v += (PEN - v) * a
            img[row + x] = int(v + 0.5)
    with open(out, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (w, h))
        f.write(img)


if __name__ == '__main__':
    main()
