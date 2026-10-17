#!/usr/bin/env python3
"""The README chart: footsteps per second other players get from a running
player, by that player's com_maxfps. Stock QL model, stock QL measured, and
footstepsVQL measured. Pillow only, like _footstep_rate_plot.py.

    python scripts/_readme_chart.py [docs/footstep-rate.png]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _footstep_rate_model as model  # noqa: E402

# Stock QL, measured 2026-09 on the test server: vanilla footstep events per
# second of running, from the server's own counters (scripts/_footstep_measured.py).
STOCK = [(125, 2.775), (200, 2.936), (241, 2.090), (297, 2.273), (332, 2.364),
         (356, 2.081), (368, 1.914), (393, 1.639), (440, 0.960), (487, 0.285),
         (972, 0.0)]
# footstepsVQL d6d28bed, measured 2026-10-03: footsteps each listener bot was
# sent, flat running, 30 s per point (RELEASE.md, Live - d6d28bed).
FIXED = [(250, 2.103), (300, 2.241), (333, 2.379), (360, 2.000), (380, 2.000),
         (400, 2.034), (450, 1.931), (500, 2.034), (750, 2.034), (990, 2.069)]

S = 2
W, H = 1400 * S, 720 * S
L, R, T, B = 104 * S, 40 * S, 112 * S, 92 * S
X0, X1, Y0, Y1 = 100.0, 1000.0, 0.0, 3.4

BG, INK, MUTED, GRID, AXIS = (255, 255, 255), (28, 32, 40), (110, 116, 128), (234, 237, 242), (150, 156, 168)
THEORY, STOCKC, FIXC, TARGETC = (150, 160, 190), (205, 64, 64), (22, 140, 100), (196, 200, 210)
TARGET = 2.10   # a real 250 fps walk: the tempo footstepsVQL's clock runs at


def font(size, bold=False):
    for name in (("segoeuib.ttf", "arialbd.ttf") if bold else ("segoeui.ttf", "arial.ttf")):
        try:
            return ImageFont.truetype(name, size * S)
        except OSError:
            continue
    return ImageFont.load_default()


F_T, F_S, F_A, F_K, F_L = font(26, True), font(16), font(16), font(15), font(16)


def px(f):
    return L + (f - X0) / (X1 - X0) * (W - L - R)


def py(r):
    return H - B - (r - Y0) / (Y1 - Y0) * (H - T - B)


def series(d, pts, color, width, dot):
    xy = [(px(f), py(r)) for f, r in pts]
    d.line(xy, fill=color, width=width * S, joint="curve")
    for x, y in xy:
        d.ellipse([x - dot * S, y - dot * S, x + dot * S, y + dot * S], fill=color, outline=BG, width=S)


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parents[1] / "docs" / "footstep-rate.png"
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    for r in (0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0):
        d.line([L, py(r), W - R, py(r)], fill=GRID, width=S)
        d.text((L - 12 * S, py(r)), "{:.1f}".format(r), font=F_K, fill=MUTED, anchor="rm")
    for f in (125, 250, 333, 400, 500, 600, 700, 800, 900, 1000):
        d.line([px(f), H - B, px(f), H - B + 6 * S], fill=AXIS, width=S)
        d.text((px(f), H - B + 10 * S), str(f), font=F_K, fill=MUTED, anchor="ma")
    d.line([L, H - B, W - R, H - B], fill=AXIS, width=S)
    d.line([L, T, L, H - B], fill=AXIS, width=S)

    x = L
    while x < W - R:
        d.line([x, py(TARGET), min(x + 10 * S, W - R), py(TARGET)], fill=TARGETC, width=2 * S)
        x += 18 * S
    theory = [(f, model.rate_ideal(f)) for f in range(125, 1001)]
    d.line([(px(f), py(r)) for f, r in theory], fill=THEORY, width=2 * S)
    series(d, STOCK, STOCKC, 3, 6)
    series(d, FIXED, FIXC, 4, 7)

    d.text((L, 30 * S), "Footsteps other players hear from a running player, by that player's fps",
           font=F_T, fill=INK)
    d.text((L, 70 * S), "Stock Quake Live goes quiet above ~400 fps and silent from 500 fps. "
           "With footstepsVQL every framerate sounds like a 250 fps player.", font=F_S, fill=MUTED)
    d.text(((L + W - R) / 2, H - 36 * S), "player's com_maxfps", font=F_A, fill=MUTED, anchor="ma")
    d.text((L - 12 * S, T - 14 * S), "footsteps / s", font=F_A, fill=MUTED, anchor="rs")

    lx, ly = px(600), py(3.2)
    for i, (c, w, label) in enumerate((
            (FIXC, 4, "with footstepsVQL - measured (what listeners were sent, 2026-10-03)"),
            (STOCKC, 3, "stock QL - measured (server counters, 2026-09)"),
            (THEORY, 2, "stock QL - model: bobCycle += floor(0.4 x msec)"),
            (TARGETC, 2, "target: a real 250 fps walk, 2.10/s"))):
        y = ly + i * 30 * S
        if c == TARGETC:
            for x in (lx - 60 * S, lx - 42 * S, lx - 24 * S):
                d.line([x, y, x + 8 * S, y], fill=c, width=w * S)
        else:
            d.line([lx - 60 * S, y, lx - 16 * S, y], fill=c, width=w * S)
        d.text((lx - 8 * S, y), label, font=F_L, fill=INK, anchor="lm")

    img = img.resize((W // S, H // S), Image.LANCZOS)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out, optimize=True)
    print("wrote {} ({}x{})".format(out, W // S, H // S))


if __name__ == "__main__":
    main()
