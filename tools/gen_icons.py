"""Generate the three format icons used by DefaultIcon (docs/architecture.md §2.4).

    uv run --with pillow python tools/gen_icons.py [--out icons] [--preview]

Writes icons/apk.ico, icons/apks.ico, icons/hap.ico; each .ico carries 16/24/32/48/256
and every size is drawn at its own resolution (no 256 -> 16 downscale, the glyph would
turn to mush).  Committed to the repo: Explorer reads the files at runtime, the build
does not depend on Python.

Design (redrawn 2026-11-04, following the reference image the user supplied; that image is
not stored in the repo - the constants below are the source of truth):
  * one silhouette for the whole family - a page with a folded corner, crossed by a
    banner that overhangs the page edges and carries the letters in white
  * colour carries the family: .apk and the multi-APK containers share Android green
    (page #AACC34, banner #5FB74B); .hap keeps its own red so the three stay apart
  * a container icon stacks two pages instead of folding one corner ("one file" vs
    "a box of files"); the letters, drawn from 48 px up, say which box
  * flat colours, no gradients: at 16 px a gradient is noise, and Explorer asks for
    16 px most of the time

The lettering is Nunito Black (SIL OFL, vendored in tools/fonts/Nunito-Variable.ttf)
because the reference uses a geometric heavy sans with round stroke ends; the system
fallback is Segoe UI Black, which is flatter but still reads.
"""

import argparse
import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - guidance only
    sys.exit("Pillow missing: run `uv run --with pillow python tools/gen_icons.py`")

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = (16, 24, 32, 48, 256)

# --- geometry, fractions of the canvas, measured off the 512x512 reference --------
SHEET = (0.125, 0.039, 0.873, 0.960)   # page box (x0, y0, x1, y1)
SHEET_RADIUS = 0.078                   # page corner radius
FOLD = 0.266                           # folded corner: legs of the 45 deg cut
FOLD_ROUND = 0.022                     # softening of the flap's three vertices
BAND_TOP = 0.4755                      # banner top, fraction of the page height
BAND_BOTTOM = 0.863                    # banner bottom, same reference
BAND_OVERHANG = 0.086                  # banner sticks out this far past the page
BAND_RADIUS = 0.070                    # banner corner radius
CAP_HEIGHT = 0.217                     # ink height of a three-letter label
LABEL_MAX_WIDTH = 0.74                 # wider labels ("XAPK") may not exceed this
STACK_OFFSET = 0.05                    # second page offset, min 1 px
LABEL_MIN_SIZE = 48                    # below this the letters are not legible

# family -> page colour, banner colour, letters, stacked?
FAMILIES = {
    "apk": dict(body="#AACC34", band="#5FB74B", label="APK", stack=False),
    "apks": dict(body="#AACC34", band="#5FB74B", label="XAPK", stack=True),
    "hap": dict(body="#FA8578", band="#C62A45", label="HAP", stack=False),
}

FONT_FILE = os.path.join(HERE, "fonts", "Nunito-Variable.ttf")
FONT_WEIGHT = 900  # Nunito ships as a variable font: pin the Black instance
FONT_FALLBACKS = tuple(
    os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name)
    for name in ("seguibl.ttf", "ariblk.ttf", "segoeuib.ttf")
)
K = 0.70710678  # cos 45 deg


def rgb(text):
    text = text.lstrip("#")
    return tuple(int(text[i:i + 2], 16) for i in (0, 2, 4))


# --- outlines (coordinates already in supersampled pixels) --------------------------

def _arc(pts, cx, cy, radius, a0, a1, n=16):
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        pts.append((cx + radius * math.cos(a), cy + radius * math.sin(a)))


def _round_corner(pts, p0, vertex, p2, n=10):
    """Quadratic Bezier p0 -> vertex -> p2, to soften one polygon vertex."""
    for i in range(n + 1):
        t = i / n
        pts.append((
            (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * vertex[0] + t ** 2 * p2[0],
            (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * vertex[1] + t ** 2 * p2[1],
        ))


def page_outline(x0, y0, x1, y1, radius, fold, fr):
    """Page: rounded rectangle whose top-right corner is cut at 45 degrees."""
    pts = [(x0 + radius, y0), (x1 - fold - fr * K, y0)]
    _round_corner(pts, (x1 - fold - fr * K, y0), (x1 - fold, y0),
                  (x1 - fold + fr * K, y0 + fr * K))
    pts.append((x1 - fr * K, y0 + fold - fr * K))
    _round_corner(pts, (x1 - fr * K, y0 + fold - fr * K), (x1, y0 + fold),
                  (x1, y0 + fold + fr))
    pts.append((x1, y1 - radius))
    _arc(pts, x1 - radius, y1 - radius, radius, 0, 90)
    pts.append((x0 + radius, y1))
    _arc(pts, x0 + radius, y1 - radius, radius, 90, 180)
    pts.append((x0, y0 + radius))
    _arc(pts, x0 + radius, y0 + radius, radius, 180, 270)
    return pts


def fold_outline(x0, y0, fold, fr):
    """The folded-over flap: the triangle above the cut, vertices softened."""
    a, b, c = (x0, y0), (x0 + fold, y0 + fold), (x0, y0 + fold)
    pts = []
    _round_corner(pts, (a[0], a[1] + fr), a, (a[0] + fr * K, a[1] + fr * K))
    _round_corner(pts, (b[0] - fr * K, b[1] - fr * K), b, (b[0] - fr, b[1]))
    _round_corner(pts, (c[0] + fr, c[1]), c, (c[0], c[1] - fr))
    return pts


# --- type ---------------------------------------------------------------------------

def _font(size, path, weight):
    font = ImageFont.truetype(path, size)
    if weight is not None:
        font.set_variation_by_axes([weight])
    return font


def label_font(label, cap_px, max_w_px, font_path):
    """Font whose ink is cap_px tall, shrunk if the label would exceed max_w_px."""
    weight = FONT_WEIGHT if font_path == FONT_FILE else None
    probe = _font(200, font_path, weight)
    left, top, right, bottom = probe.getbbox(label)
    size = max(1, round(200 * cap_px / max(1, bottom - top)))
    font = _font(size, font_path, weight)
    left, top, right, bottom = font.getbbox(label)
    if right - left > max_w_px:
        size = max(1, round(size * max_w_px / (right - left)))
        font = _font(size, font_path, weight)
    return font


def resolve_font(explicit):
    for candidate in (explicit, FONT_FILE, *FONT_FALLBACKS):
        if candidate and os.path.exists(candidate):
            return candidate
    return None


# --- rendering ----------------------------------------------------------------------

def render(family, size, font_path):
    spec = FAMILIES[family]
    body, band = rgb(spec["body"]), rgb(spec["band"])
    ss = 8 if size <= 48 else 4  # supersample hard where the pixels are few
    s = size * ss
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    x0, y0, x1, y1 = (v * s for v in SHEET)
    radius = SHEET_RADIUS * s
    # at 16/24 px a proportional offset rounds to 1 px and the two pages melt
    # together, so the small sizes get a fixed 2 px step
    off_px = max(1, round(STACK_OFFSET * size))
    if size <= 32:
        off_px = max(2, off_px)
    off = off_px * ss

    if spec["stack"]:
        # two pages: the back one peeks out top-right, so the pair reads as a stack
        draw.rounded_rectangle((x0 + off, y0, x1, y1 - off), radius=radius, fill=band)
        fx0, fy0, fx1, fy1 = x0, y0 + off, x1 - off, y1
        draw.rounded_rectangle((fx0, fy0, fx1, fy1), radius=radius, fill=body)
    else:
        fx0, fy0, fx1, fy1 = x0, y0, x1, y1
        draw.polygon(page_outline(x0, y0, x1, y1, radius, FOLD * s, FOLD_ROUND * s),
                     fill=body)
        draw.polygon(fold_outline(x1 - FOLD * s, y0, FOLD * s, FOLD_ROUND * s), fill=band)

    page_h = fy1 - fy0
    bx0, bx1 = fx0 - BAND_OVERHANG * s, fx1 + BAND_OVERHANG * s
    by0, by1 = fy0 + BAND_TOP * page_h, fy0 + BAND_BOTTOM * page_h
    draw.rounded_rectangle((bx0, by0, bx1, by1), radius=BAND_RADIUS * s, fill=band)

    if size >= LABEL_MIN_SIZE and font_path:
        font = label_font(spec["label"], CAP_HEIGHT * s, LABEL_MAX_WIDTH * s, font_path)
        left, top, right, bottom = draw.textbbox((0, 0), spec["label"], font=font)
        draw.text(((bx0 + bx1) / 2 - (left + right) / 2,
                   (by0 + by1) / 2 - (top + bottom) / 2),
                  spec["label"], font=font, fill=(255, 255, 255, 255))

    # premultiplied downscale: keeps the transparent margin free of dark fringes
    return img.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def build(out_dir, font_path):
    frames = {}
    for family in FAMILIES:
        frames[family] = [render(family, size, font_path) for size in SIZES]
        path = os.path.join(out_dir, family + ".ico")
        frames[family][-1].save(
            path,
            format="ICO",
            sizes=[(s, s) for s in SIZES],
            append_images=frames[family][:-1],
        )
        print(f"wrote {path} ({os.path.getsize(path)} bytes, sizes {SIZES})")
    return frames


def preview(out_dir, frames):
    """Contact sheet: every family on a light and a dark background, 1:1 and 4x."""
    pad, gap, zoom = 16, 18, 4
    small = (48, 32, 24, 16)
    strip_w = sum(max(n, n * zoom) for n in small) + gap * (len(small) - 1)
    cell_w = pad + 256 + gap + strip_w + pad
    row_h = 256 + pad
    panel_h = row_h * len(FAMILIES) + pad
    sheet = Image.new("RGBA", (cell_w, panel_h * 2 + pad), (255, 255, 255, 255))
    y = pad
    for background in ((247, 247, 247, 255), (26, 28, 32, 255)):
        sheet.paste(Image.new("RGBA", (cell_w, panel_h), background), (0, y - pad))
        for family in FAMILIES:
            sheet.alpha_composite(frames[family][SIZES.index(256)], (pad, y))
            x = pad + 256 + gap
            for size in small:
                one = frames[family][SIZES.index(size)]
                sheet.alpha_composite(one, (x, y))
                sheet.alpha_composite(one.resize((size * zoom, size * zoom), Image.NEAREST),
                                      (x, y + 64))
                x += max(size, size * zoom) + gap
            y += row_h
    out = os.path.join(out_dir, "_preview.png")
    sheet.save(out)
    print(f"wrote {out}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "icons"))
    parser.add_argument("--font", default=None,
                        help=f"override the label font (default: {os.path.basename(FONT_FILE)})")
    parser.add_argument("--preview", action="store_true",
                        help="also write icons/_preview.png (contact sheet for review)")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    font_path = resolve_font(args.font)
    if font_path is None:
        sys.exit("no label font found (tools/fonts/Nunito-Variable.ttf and the system "
                 "fallbacks are all missing); pass --font <file.ttf>")
    if font_path != FONT_FILE:
        print(f"warning: fallback font {font_path}: glyphs will not match the reference")

    frames = build(args.out, font_path)
    if args.preview:
        preview(args.out, frames)


if __name__ == "__main__":
    main()
