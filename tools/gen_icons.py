"""Generate the three format icons used by DefaultIcon (docs/architecture.md §2.4).

    uv run --with pillow python tools/gen_icons.py [--out icons] [--preview]

Writes icons/apk.ico, icons/apks.ico, icons/hap.ico; each .ico carries 16/24/32/48/256
and every size is drawn at its own resolution (no 256 -> 16 downscale, the glyph would
turn to mush).  Committed to the repo: Explorer reads the files at runtime, the build
does not depend on Python.

Design rules (why it looks like this):
  * colour carries the family  (APK green / multi-APK indigo / HAP red)
  * the glyph carries the kind  (robot head / stacked packages / hexagon ring)
  * text is only drawn from 48 px up; below that three letterforms are not legible
  * everything is white on the coloured tile so it survives both light and dark
    Explorer themes, and the tile keeps a 1 px margin so 16 px icons do not clip.
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - guidance only
    sys.exit("Pillow missing: run `uv run --with pillow python tools/gen_icons.py`")

SIZES = (16, 24, 32, 48, 256)

# family -> (top colour, bottom colour, label, glyph)
FAMILIES = {
    "apk": ("#7BE8A6", "#1E9E5A", "APK", "robot"),
    "apks": ("#9C8CFF", "#4F46E5", "XAPK", "stack"),
    "hap": ("#FF8A7A", "#C81E4A", "HAP", "hex"),
}

BOLD_FONTS = (
    r"C:\Windows\Fonts\segoeuib.ttf",
    r"C:\Windows\Fonts\arialbd.ttf",
)


def rgb(text):
    text = text.lstrip("#")
    return tuple(int(text[i : i + 2], 16) for i in (0, 2, 4))


def rounded_tile(draw, box, radius, top, bottom, ss):
    """Vertical gradient clipped to a rounded rectangle (drawn in supersampled space)."""
    x0, y0, x1, y1 = box
    h = y1 - y0
    grad = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        grad.putpixel(
            (0, y),
            tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3)),
        )
    grad = grad.resize((x1 - x0, y1 - y0), Image.NEAREST)
    mask = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, x1 - x0 - 1, y1 - y0 - 1), radius=radius, fill=255
    )
    draw._image.paste(grad, (x0, y0), mask)


def glyph_robot(draw, cx, cy, r, white, accent):
    """Android head: dome + two antennae + two eyes."""
    dome_top = cy - r * 0.15
    draw.pieslice(
        (cx - r, dome_top - r, cx + r, dome_top + r), start=180, end=360, fill=white
    )
    draw.rectangle(
        (cx - r, dome_top, cx + r, dome_top + r * 0.78), fill=white
    )
    eye_r = max(1, r * 0.11)
    eye_y = dome_top + r * 0.42
    for dx in (-r * 0.42, r * 0.42):
        draw.ellipse(
            (cx + dx - eye_r, eye_y - eye_r, cx + dx + eye_r, eye_y + eye_r),
            fill=accent,
        )
    w = max(1, int(r * 0.12))
    for dx in (-0.62, 0.62):
        draw.line(
            (cx + r * dx * 0.55, dome_top - r * 0.72, cx + r * dx, dome_top - r * 1.12),
            fill=white,
            width=w,
        )


def glyph_stack(draw, cx, cy, r, white, accent):
    """Two fanned cards: "one container, several APKs".

    Filled shapes only (no inner knockout) — a knockout turned into a "clipboard"
    at 16 px, while two diagonally offset tiles keep their silhouette at every size.
    """
    card = r * 0.86
    rad = card * 0.34
    off = r * 0.40
    back = (cx - card + off, cy - card - off, cx + card + off, cy + card - off)
    front = (cx - card - off, cy - card + off, cx + card - off, cy + card + off)
    draw.rounded_rectangle(back, radius=rad, fill=white)
    # 前卡描一圈底色：16 px 下两张卡并成一块白，靠这条缝才看得出"两张"
    draw.rounded_rectangle(front, radius=rad, fill=white, outline=accent,
                           width=max(1, round(r * 0.16)))


def glyph_hex(draw, cx, cy, r, white, accent):
    """Hexagon ring: the HarmonyOS shape, without the logo."""
    import math

    def hexagon(radius):
        return [
            (cx + radius * math.cos(math.radians(a)), cy + radius * math.sin(math.radians(a)))
            for a in (30, 90, 150, 210, 270, 330)
        ]

    draw.polygon(hexagon(r), fill=white)
    draw.polygon(hexagon(r * 0.56), fill=accent)


def render(family, size, label_font_path):
    top_hex, bottom_hex, label, glyph = FAMILIES[family]
    top, bottom = rgb(top_hex), rgb(bottom_hex)
    ss = 8 if size <= 48 else 2  # supersample the small ones hard, the big one mildly
    s = size * ss

    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw._image = img

    margin = max(1, round(s * 0.015))
    radius = round(s * 0.20)
    box = (margin, margin, s - margin, s - margin)
    rounded_tile(draw, box, radius, top, bottom, ss)
    # 1 px lighter rim keeps the tile from melting into a white background
    draw.rounded_rectangle(box, radius=radius, outline=(255, 255, 255, 60),
                           width=max(1, round(s * 0.012)))

    white = (255, 255, 255, 255)
    cx = s / 2
    if size >= 48:
        r = s * 0.20
        cy = s * 0.40
    else:
        r = s * 0.27
        cy = s * 0.50

    if glyph == "robot":
        glyph_robot(draw, cx, cy, r, white, bottom + (255,))
    elif glyph == "stack":
        glyph_stack(draw, cx, cy, r * 0.85, white, bottom + (255,))
    else:
        glyph_hex(draw, cx, cy, r, white, bottom + (255,))

    if size >= 48:
        font_size = round(s * 0.155) if size < 128 else round(s * 0.135)
        font = None
        for path in (label_font_path, *BOLD_FONTS):
            if path and os.path.exists(path):
                font = ImageFont.truetype(path, font_size)
                break
        if font is None:
            font = ImageFont.load_default()
        tracking = font_size * 0.06
        widths = [draw.textlength(ch, font=font) for ch in label]
        total = sum(widths) + tracking * (len(label) - 1)
        x = cx - total / 2
        baseline = s * 0.745
        for ch, w in zip(label, widths):
            draw.text((x, baseline), ch, font=font, fill=white, anchor="lm")
            x += w + tracking

    return img.resize((size, size), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join("icons"))
    parser.add_argument("--font", default=BOLD_FONTS[0])
    parser.add_argument("--preview", action="store_true",
                        help="also write icons/_preview.png (contact sheet for review)")
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for family in FAMILIES:
        frames = [render(family, size, args.font) for size in SIZES]
        path = os.path.join(args.out, family + ".ico")
        frames[-1].save(
            path,
            format="ICO",
            sizes=[(s, s) for s in SIZES],
            append_images=frames[:-1],
        )
        print(f"wrote {path} ({os.path.getsize(path)} bytes, sizes {SIZES})")

    if args.preview:
        pad, row_h = 12, 260
        sheet = Image.new("RGBA", (len(SIZES) * (256 + pad) + pad, len(FAMILIES) * row_h + pad),
                          (32, 34, 38, 255))
        for row, family in enumerate(FAMILIES):
            for col, size in enumerate(SIZES):
                frame = render(family, 256, args.font).copy()
                small = render(family, size, args.font)
                frame.paste(small, (8 + col * 40, 8), small)
                # also paste the real size 1:1 so aliasing is visible
                x = pad + col * (256 + pad)
                y = pad + row * row_h
                sheet.paste(frame, (x, y))
                one = render(family, size, args.font)
                sheet.paste(one, (x + 256 - size - 4, y + row_h - size - 4), one)
        out = os.path.join(args.out, "_preview.png")
        sheet.save(out)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
