#!/usr/bin/env python3
"""The icon of the app, drawn from the mark of its chain (chain.json: brand.mark, brand.color).

The window, the taskbar, the tray and the desktop entry showed the Bitcoin coin in every app (user feedback
2026-10-19). This draws the chain's own mark (the one of the title bar, components/Brand.svelte: SVG paths on
a 32 grid, stroked) in white on a rounded tile of the chain's colour, and writes:

  src/webui/app/public/icon-256.png  the window and the tray on Linux (tray_gtk.cpp AppIcon), the desktop
                                     entry's icon (installed as <bin>.png in hicolor/256x256/apps)
  src/webui/res/icons/app.ico        the program, its window and its tray icon on Windows (chains-app-res.rc),
                                     the installer

Run it after a change of brand.mark or brand.color (from the root of the repository):
  python3 src/webui/app/scripts/appicon.py [--brand '{"mark": [...], "color": "#..."}']
Needs Pillow. The paths may use M L H V C A Z, absolute or relative, with implicit repeats.
"""
import argparse
import json
import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[4]
SCALE = 1024  # drawn large, then scaled down (antialiasing)


def tokens(d):
    for m in re.finditer(r'[MmLlHhVvCcAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?', d):
        yield m.group(0)


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2, steps=24):
    """The SVG arc from (x1, y1) to (x2, y2) as points (SVG 1.1 F.6.5, endpoint to center)."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    xp, yp = c * dx + s * dy, -s * dx + c * dy
    lam = xp * xp / (rx * rx) + yp * yp / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    k = math.sqrt(max(0.0, num / den)) if den else 0.0
    if large == sweep:
        k = -k
    cxp, cyp = k * rx * yp / ry, -k * ry * xp / rx
    cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    dt = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    out = []
    for i in range(1, steps + 1):
        t = t1 + dt * i / steps
        out.append((cx + rx * math.cos(t) * c - ry * math.sin(t) * s, cy + rx * math.cos(t) * s + ry * math.sin(t) * c))
    return out


def subpaths(d):
    """The polylines of a path: [(points, closed)]."""
    toks = list(tokens(d))
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    out, cur = [], []

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flush(closed=False):
        nonlocal cur
        if len(cur) > 1:
            out.append((cur, closed))
        cur = []

    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                cur.append((sx, sy))
                flush(True)
                x, y = sx, sy
                cur = [(x, y)]
                continue
        rel = cmd.islower()
        C = cmd.upper()
        if C == 'M':
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            flush()
            x, y, sx, sy = nx, ny, nx, ny
            cur = [(x, y)]
            cmd = 'l' if rel else 'L'
        elif C == 'L':
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
        elif C == 'H':
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
        elif C == 'V':
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
        elif C == 'C':
            p = [num() for _ in range(6)]
            if rel:
                p = [p[0] + x, p[1] + y, p[2] + x, p[3] + y, p[4] + x, p[5] + y]
            x0, y0 = x, y
            for k in range(1, 17):
                t = k / 16
                a, b, c_, e = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
                cur.append((a * x0 + b * p[0] + c_ * p[2] + e * p[4], a * y0 + b * p[1] + c_ * p[3] + e * p[5]))
            x, y = p[4], p[5]
        elif C == 'A':
            rx, ry, phi, large, sweep, nx, ny = num(), num(), num(), num(), num(), num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            cur.extend(arc_points(x, y, rx, ry, phi, int(large), int(sweep), nx, ny))
            x, y = nx, ny
        else:
            raise SystemExit(f'appicon: path command {cmd} not handled in {d!r}')
    flush()
    return out


def draw(mark, color):
    img = Image.new('RGBA', (SCALE, SCALE), (0, 0, 0, 0))
    g = ImageDraw.Draw(img)
    pad = SCALE * 0.04
    rgb = tuple(int(color.lstrip('#')[k:k + 2], 16) for k in (0, 2, 4))
    # The tile: the chain's colour, a little lighter at the top.
    tile = Image.new('RGBA', (SCALE, SCALE), (0, 0, 0, 0))
    grad = Image.new('RGBA', (1, SCALE))
    for yy in range(SCALE):
        f = 1.12 - 0.24 * yy / SCALE
        grad.putpixel((0, yy), tuple(min(255, int(ch * f)) for ch in rgb) + (255,))
    grad = grad.resize((SCALE, SCALE))
    mask = Image.new('L', (SCALE, SCALE), 0)
    ImageDraw.Draw(mask).rounded_rectangle([pad, pad, SCALE - pad, SCALE - pad], radius=SCALE * 0.22, fill=255)
    tile.paste(grad, (0, 0), mask)
    img = Image.alpha_composite(img, tile)
    g = ImageDraw.Draw(img)
    # The mark on its 32 grid, in the middle three quarters of the tile, stroked in white with round joins.
    inner = SCALE * 0.74
    off = (SCALE - inner) / 2
    unit = inner / 32
    width = 2.6 * unit
    white = (255, 255, 255, 255)
    for d in mark:
        for pts, closed in subpaths(d):
            px = [(off + px_ * unit, off + py_ * unit) for px_, py_ in pts]
            for a, b in zip(px, px[1:]):
                g.line([a, b], fill=white, width=int(round(width)))
            for p in px:
                g.ellipse([p[0] - width / 2, p[1] - width / 2, p[0] + width / 2, p[1] + width / 2], fill=white)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--brand', help='the brand as JSON ({"mark": [...], "color": "#rrggbb"}) instead of chain.json')
    ap.add_argument('--root', default=str(ROOT), help='the root of the repository')
    args = ap.parse_args()
    root = Path(args.root)
    brand = json.loads(args.brand) if args.brand else json.loads((root / 'src/webui/app/src/chain/chain.json').read_text())['brand']
    big = draw(brand['mark'], brand['color'])
    png = root / 'src/webui/app/public/icon-256.png'
    big.resize((256, 256), Image.LANCZOS).save(png, optimize=True)
    ico = root / 'src/webui/res/icons/app.ico'
    big.resize((256, 256), Image.LANCZOS).save(ico, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(f'appicon: {png.relative_to(root)}, {ico.relative_to(root)}')


if __name__ == '__main__':
    main()
