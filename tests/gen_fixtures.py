#!/usr/bin/env python3
"""Generate real APK test fixtures for apk-shell-ext.

Every "good" fixture is built by the Android SDK's aapt2 from sources under
tests/fixtures/src/<name>/ (AndroidManifest.xml + res/), so the resulting
AndroidManifest.xml is genuine AXML and resources.arsc is a genuine AOSP
resource table -- not hand-rolled bytes.  The only hand-written ZIP logic is
for the adversarial samples in generated/bad/, which must be malformed in
ways no packaging tool would ever produce.

Stdlib only (zipfile, zlib, struct, json, subprocess, pathlib, shutil, sys).
PNG rasters are written by a tiny deterministic RGBA writer; the single WebP
raster is encoded by ffmpeg (libwebp).

Run:  python tests/gen_fixtures.py [--keep-build]
      AAPT2=<path> ANDROID_JAR=<path> override SDK discovery.

The script is idempotent: it deletes tests/fixtures/generated/, rebuilds every
fixture, rewrites tests/fixtures/MANIFEST.json, then verifies its own output
(exit code 1 on any failed assertion).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

# --------------------------------------------------------------------------
# paths / tool discovery
# --------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURES = REPO_ROOT / "tests" / "fixtures"
SRC = FIXTURES / "src"
GEN = FIXTURES / "generated"
BAD = GEN / "bad"
REAL = FIXTURES / "real"
MANIFEST_PATH = FIXTURES / "MANIFEST.json"

DEFAULT_AAPT2 = "E:/PROGRAMS/Android/Sdk/build-tools/36.0.0/aapt2.exe"
DEFAULT_ANDROID_JAR = "E:/PROGRAMS/Android/Sdk/platforms/android-36/android.jar"

MIN_SDK = 26
TARGET_SDK = 34

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# mdpi..xxxhdpi sizes for a 48dp launcher icon (48 * scale).
DENSITIES = [
    ("mdpi", 1, 48),
    ("hdpi", 2, 72),
    ("xhdpi", 3, 96),
    ("xxhdpi", 4, 144),
    ("xxxhdpi", 5, 192),
]
DENSITY_BY_NAME = {name: (scale, px) for name, scale, px in DENSITIES}

# Flat, fully opaque colours chosen so a centre-pixel probe identifies the
# density that was picked.
PNG_COLORS = {
    "mdpi": (0xFF, 0x00, 0x00),
    "hdpi": (0x00, 0xFF, 0x00),
    "xhdpi": (0x00, 0x00, 0xFF),
    "xxhdpi": (0xFF, 0xFF, 0x00),
    "xxxhdpi": (0xFF, 0x00, 0xFF),
}

WEBP_COLOR = (0x00, 0xFF, 0xFF)      # #00FFFF, xxhdpi only
ADAPTIVE_BG = (0xFF, 0x00, 0x66)     # #FF0066, values/colors.xml
ADAPTIVE_FG = (0xFF, 0x99, 0x00)     # #FF9900, 432px foreground raster
ADAPTIVE_FALLBACKS = {               # raster fallbacks
    "xxxhdpi": (0x00, 0x66, 0xFF),   # #0066FF
    "xxhdpi": (0x66, 0xFF, 0x00),    # #66FF00
}


def tool_paths() -> tuple[Path, Path, str]:
    aapt2 = Path(os.environ.get("AAPT2") or DEFAULT_AAPT2)
    android_jar = Path(os.environ.get("ANDROID_JAR") or DEFAULT_ANDROID_JAR)
    ffmpeg = shutil.which("ffmpeg") or "ffmpeg"
    return aapt2, android_jar, ffmpeg


def run(cmd: list[str], *, check: bool = True) -> subprocess.CompletedProcess:
    printable = " ".join(str(c) for c in cmd)
    print(f"    $ {printable}")
    proc = subprocess.run(
        [str(c) for c in cmd],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and proc.returncode != 0:
        sys.stderr.write(proc.stdout.decode("utf-8", "replace"))
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        raise SystemExit(f"command failed ({proc.returncode}): {printable}")
    return proc


# --------------------------------------------------------------------------
# deterministic PNG writer / reader (stdlib only)
# --------------------------------------------------------------------------

def png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def solid_png(width: int, height: int, rgb: tuple[int, int, int]) -> bytes:
    """8-bit RGBA PNG, every pixel (r,g,b,255), filter type 0 on every row."""
    r, g, b = rgb
    row = b"\x00" + bytes((r, g, b, 0xFF)) * width
    raw = row * height
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", ihdr)
        + png_chunk(b"IDAT", zlib.compress(raw, 9))
        + png_chunk(b"IEND", b"")
    )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png_rgba(data: bytes) -> tuple[int, int, bytes]:
    """Decode an 8-bit non-interlaced PNG to (w, h, RGBA bytes).

    Supports colour types 6 (RGBA), 2 (RGB), 0 (grey); filters 0-4.  Enough
    to verify our own output and to prove aapt2 did not re-encode it.
    """
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, ihdr = 8, bytearray(), None
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", payload)
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if ihdr is None:
        raise ValueError("no IHDR")
    width, height, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or interlace != 0:
        raise ValueError(f"unsupported PNG (depth={depth} interlace={interlace})")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise ValueError(f"unsupported PNG colour type {ctype} (palette?)")
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ftype == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                upleft = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif ftype != 0:
            raise ValueError(f"bad PNG filter {ftype}")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if channels != 4:
        rgba = bytearray(width * height * 4)
        for i in range(width * height):
            px = out[i * channels:(i + 1) * channels]
            if channels == 1:
                rgba[i * 4:i * 4 + 4] = bytes((px[0], px[0], px[0], 0xFF))
            elif channels == 2:
                rgba[i * 4:i * 4 + 4] = bytes((px[0], px[0], px[0], px[1]))
            else:
                rgba[i * 4:i * 4 + 4] = bytes((px[0], px[1], px[2], 0xFF))
        out = rgba
    return width, height, bytes(out)


def rgba_hex(rgba: bytes) -> str:
    return rgba.hex().upper()


# --------------------------------------------------------------------------
# hand-rolled ZIP builder (adversarial samples only)
# --------------------------------------------------------------------------

class ZipEntry:
    __slots__ = ("name", "payload", "method", "usize", "crc", "precompressed")

    def __init__(self, name: str, payload: bytes, *, method: int = 8,
                 usize: int | None = None, crc: int | None = None,
                 precompressed: bool = False):
        self.name = name
        self.payload = payload          # raw file bytes, or deflate stream if precompressed
        self.method = method
        self.precompressed = precompressed
        self.usize = len(payload) if usize is None else usize
        self.crc = (zlib.crc32(payload) & 0xFFFFFFFF) if crc is None else crc

    def stored(self) -> tuple[bytes, int]:
        """Return (bytes-in-zip, compressed-size) for this entry."""
        if self.precompressed or self.method == 0:
            return self.payload, len(self.payload)
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        stream = co.compress(self.payload) + co.flush()
        return stream, len(stream)


def build_zip(entries: list[ZipEntry], *, zip64_force: bool = False) -> bytes:
    """Minimal but spec-shaped ZIP writer (store/deflate, optional ZIP64).

    ZIP64 EOCD is emitted when the entry count exceeds 65535, when any offset
    or size exceeds 32 bits, or when zip64_force is set.  This is what lets us
    build a 70000-entry archive at all: plain EOCD only has 16-bit counters.
    """
    out = bytearray()
    central = bytearray()
    count = 0
    for e in entries:
        data, csize = e.stored()
        name = e.name.encode("utf-8")
        offset = len(out)
        dos_time, dos_date = 0, 0x0021  # 1980-01-01 00:00:00, fixed
        out += struct.pack(
            "<IHHHHHIIIHH", 0x04034B50, 20, 0, e.method, dos_time, dos_date,
            e.crc, csize, e.usize, len(name), 0,
        )
        out += name
        out += data
        central += struct.pack(
            "<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, e.method, dos_time,
            dos_date, e.crc, csize, e.usize, len(name), 0, 0, 0, 0, 0, offset,
        )
        central += name
        count += 1

    cd_offset = len(out)
    out += central
    cd_size = len(central)

    need_zip64 = (
        zip64_force
        or count > 0xFFFF
        or cd_offset > 0xFFFFFFFF
        or cd_size > 0xFFFFFFFF
        or any(e.usize > 0xFFFFFFFF or e.stored()[1] > 0xFFFFFFFF for e in entries)
    )
    if need_zip64:
        z64_off = len(out)
        out += struct.pack(
            "<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0,
            count, count, cd_size, cd_offset,
        )
        out += struct.pack("<IIQI", 0x07064B50, 0, z64_off, 1)
        out += struct.pack(
            "<IHHHHIIH", 0x06054B50, 0, 0,
            0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0,
        )
    else:
        out += struct.pack(
            "<IHHHHIIH", 0x06054B50, 0, 0, count, count, cd_size, cd_offset, 0,
        )
    return bytes(out)


def zip_count(data: bytes) -> int:
    """Entry count from EOCD, preferring the ZIP64 record when present."""
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0:
        raise ValueError("no EOCD")
    total16 = struct.unpack("<H", data[eocd + 10:eocd + 12])[0]
    loc = data.rfind(b"PK\x06\x07", 0, eocd)
    if loc >= 0:
        z64 = struct.unpack("<Q", data[loc + 8:loc + 16])[0]
        if data[z64:z64 + 4] == b"PK\x06\x06":
            return struct.unpack("<Q", data[z64 + 32:z64 + 40])[0]
    return total16


# --------------------------------------------------------------------------
# source materialisation
# --------------------------------------------------------------------------

def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def manifest_xml(package: str, version_code: int, version_name: str,
                 label: str, icon: str | None, *, has_activity: bool = True) -> str:
    icon_attr = f'\n        android:icon="{icon}"' if icon else ""
    activity = ""
    if has_activity:
        activity = f"""
        <activity
            android:name=".MainActivity"
            android:exported="true"
            android:label="@string/app_name">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>"""
    return f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="{ANDROID_NS}"
    package="{package}"
    android:versionCode="{version_code}"
    android:versionName="{version_name}">

    <application
        android:label="@string/app_name"
        android:hasCode="false"{icon_attr}>{activity}
    </application>
</manifest>
"""


def strings_xml(value: str) -> str:
    return f"""<?xml version="1.0" encoding="utf-8"?>
<resources>
    <string name="app_name">{value}</string>
</resources>
"""


# --------------------------------------------------------------------------
# fixture builders
# --------------------------------------------------------------------------

def build_f_png() -> dict:
    root = SRC / "f_png"
    res = root / "res"
    write_text(root / "AndroidManifest.xml",
               manifest_xml("com.example.pngicon", 10203, "1.2.3",
                            "PNG Icon Demo", "@mipmap/ic_launcher"))
    write_text(res / "values" / "strings.xml", strings_xml("PNG Icon Demo"))
    write_text(res / "values-zh-rCN" / "strings.xml", strings_xml("中文标签示例"))
    for name, _, px in DENSITIES:
        write_bytes(res / f"mipmap-{name}" / "ic_launcher.png",
                    solid_png(px, px, PNG_COLORS[name]))
    return {"name": "f_png", "src": root, "package": "com.example.pngicon",
            "versionCode": 10203, "versionName": "1.2.3",
            "labelDefault": "PNG Icon Demo", "labelZhCN": "中文标签示例",
            "iconResourceName": "mipmap/ic_launcher", "iconAttr": "@mipmap/ic_launcher"}


def build_f_webp(ffmpeg: str) -> dict:
    root = SRC / "f_webp"
    res = root / "res"
    write_text(root / "AndroidManifest.xml",
               manifest_xml("com.example.webpicon", 2001, "2.0.1",
                            "WebP Icon Demo", "@mipmap/ic_launcher"))
    write_text(res / "values" / "strings.xml", strings_xml("WebP Icon Demo"))
    target = res / "mipmap-xxhdpi" / "ic_launcher.webp"
    target.parent.mkdir(parents=True, exist_ok=True)
    # Lossless *and* bgra input: libwebp's yuv420p path would round #00FFFF
    # to #01FFFF.  Still encoded by ffmpeg's libwebp encoder.
    run([ffmpeg, "-y", "-v", "error", "-f", "lavfi",
         "-i", f"color=c=0x{WEBP_COLOR_HEX}:s=144x144:d=1",
         "-frames:v", "1", "-c:v", "libwebp", "-lossless", "1",
         "-pix_fmt", "bgra", str(target)])
    return {"name": "f_webp", "src": root, "package": "com.example.webpicon",
            "versionCode": 2001, "versionName": "2.0.1",
            "labelDefault": "WebP Icon Demo", "labelZhCN": None,
            "iconResourceName": "mipmap/ic_launcher", "iconAttr": "@mipmap/ic_launcher"}


def build_f_noicon() -> dict:
    root = SRC / "f_noicon"
    write_text(root / "AndroidManifest.xml",
               manifest_xml("com.example.noicon", 3001, "3.0.0",
                            "No Icon Demo", None))
    write_text(root / "res" / "values" / "strings.xml", strings_xml("No Icon Demo"))
    return {"name": "f_noicon", "src": root, "package": "com.example.noicon",
            "versionCode": 3001, "versionName": "3.0.0",
            "labelDefault": "No Icon Demo", "labelZhCN": None,
            "iconResourceName": None, "iconAttr": None}


def build_f_adaptive() -> dict:
    root = SRC / "f_adaptive"
    res = root / "res"
    write_text(root / "AndroidManifest.xml",
               manifest_xml("com.example.adaptiveicon", 4001, "4.1.0",
                            "Adaptive Icon Demo", "@mipmap/ic_launcher"))
    write_text(res / "values" / "strings.xml", strings_xml("Adaptive Icon Demo"))
    write_text(res / "values" / "colors.xml", f"""<?xml version="1.0" encoding="utf-8"?>
<resources>
    <color name="ic_launcher_bg">#{ADAPTIVE_BG_HEX}</color>
</resources>
""")
    write_text(res / "mipmap-anydpi-v26" / "ic_launcher.xml",
               f"""<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="{ANDROID_NS}">
    <background android:drawable="@color/ic_launcher_bg" />
    <foreground android:drawable="@mipmap/ic_launcher_fg" />
</adaptive-icon>
""")
    # 432px = 108dp foreground at xxxhdpi.
    write_bytes(res / "mipmap-xxxhdpi" / "ic_launcher_fg.png",
                solid_png(432, 432, ADAPTIVE_FG))
    for density, rgb in ADAPTIVE_FALLBACKS.items():
        px = DENSITY_BY_NAME[density][1]
        write_bytes(res / f"mipmap-{density}" / "ic_launcher.png",
                    solid_png(px, px, rgb))
    return {"name": "f_adaptive", "src": root, "package": "com.example.adaptiveicon",
            "versionCode": 4001, "versionName": "4.1.0",
            "labelDefault": "Adaptive Icon Demo", "labelZhCN": None,
            "iconResourceName": "mipmap/ic_launcher", "iconAttr": "@mipmap/ic_launcher"}


PNG_HEX = {k: "%02X%02X%02X" % v for k, v in PNG_COLORS.items()}
WEBP_COLOR_HEX = "%02X%02X%02X" % WEBP_COLOR
ADAPTIVE_BG_HEX = "%02X%02X%02X" % ADAPTIVE_BG


def compile_and_link(spec: dict, aapt2: Path, android_jar: Path) -> Path:
    """aapt2 compile --dir ... && aapt2 link ... -> generated/<name>.apk"""
    name = spec["name"]
    work = GEN / ".build" / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    compiled = work / "compiled.zip"
    out = GEN / f"{name}.apk"
    run([aapt2, "compile", "--no-crunch", "--dir", str(spec["src"] / "res"),
         "-o", str(compiled)])
    run([aapt2, "link", "-o", str(out),
         "--manifest", str(spec["src"] / "AndroidManifest.xml"),
         "-I", str(android_jar),
         "--min-sdk-version", str(MIN_SDK),
         "--target-sdk-version", str(TARGET_SDK),
         "--auto-add-overlay", str(compiled)])
    return out


# --------------------------------------------------------------------------
# adversarial samples
# --------------------------------------------------------------------------

def deterministic_bytes(n: int, seed: int = 0xA5) -> bytes:
    """xorshift32 stream -- stable across Python versions (unlike random)."""
    out = bytearray(n)
    x = seed & 0xFFFFFFFF
    for i in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out[i] = (x >> 11) & 0xFF
    return bytes(out)


def rewrite_zip(src: Path, dst: Path, replacements: dict[str, bytes]) -> None:
    """Copy src APK to dst, replacing named entries with new byte payloads.

    ZipInfo timestamps are pinned to 1980-01-01 so repeated runs are
    byte-identical (zipfile defaults to "now", which would break that).
    """
    import zipfile
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_STORED) as zout:
        for info in zin.infolist():
            zi = zipfile.ZipInfo(info.filename, date_time=(1980, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            if info.filename in replacements:
                zout.writestr(zi, replacements[info.filename])
            else:
                zout.writestr(zi, zin.read(info.filename))


def build_bad() -> list[dict]:
    f_png = GEN / "f_png.apk"
    raw = f_png.read_bytes()
    samples = []

    write_bytes(BAD / "empty.apk", b"")
    samples.append({"name": "empty", "file": BAD / "empty.apk", "bytes": 0,
                    "description": "0-byte file"})

    trunc = raw[:int(len(raw) * 0.6)]
    write_bytes(BAD / "trunc.apk", trunc)
    samples.append({"name": "trunc", "file": BAD / "trunc.apk", "bytes": len(trunc),
                    "truncatedFrom": len(raw), "description": "f_png.apk cut at 60%"})

    noise = deterministic_bytes(4096)
    write_bytes(BAD / "notzip.apk", noise)
    samples.append({"name": "notzip", "file": BAD / "notzip.apk", "bytes": 4096,
                    "description": "4 KiB deterministic pseudo-random bytes"})

    # 70000 zero-byte stored entries.  Names use the shortest base62 strings so
    # the archive stays as small as possible (~5.5 MB is unavoidable: 76 bytes
    # of headers per entry).
    def b62(i: int) -> str:
        alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
        s = ""
        while True:
            i, r = divmod(i, 62)
            s = alphabet[r] + s
            if i == 0:
                break
        return s

    entries = [ZipEntry(f"e/{b62(i)}", b"", method=0) for i in range(70000)]
    bomb_entries = build_zip(entries)
    write_bytes(BAD / "bomb_entries.apk", bomb_entries)
    samples.append({"name": "bomb_entries", "file": BAD / "bomb_entries.apk",
                    "bytes": len(bomb_entries), "entryCount": 70000,
                    "description": "70000 zero-byte entries, stored, ZIP64 EOCD"})

    # Path traversal + an icon entry whose deflate stream is garbage, so the
    # central directory parses but extraction fails.
    garbage_deflate = b"\xff" * 512
    traversal = build_zip([
        ZipEntry("../../evil.dll", b"MZ\x90\x00hostile payload", method=8),
        ZipEntry("res/mipmap-xxxhdpi/ic_launcher.png", garbage_deflate,
                 method=8, usize=4096, crc=0xDEADBEEF, precompressed=True),
        ZipEntry("AndroidManifest.xml", b"\x03\x00\x08\x00not really axml", method=0),
    ])
    write_bytes(BAD / "traversal.apk", traversal)
    samples.append({"name": "traversal", "file": BAD / "traversal.apk",
                    "bytes": len(traversal),
                    "entries": ["../../evil.dll", "res/mipmap-xxxhdpi/ic_launcher.png"],
                    "description": "traversal name + unreadable (invalid deflate) icon entry"})

    # 64 MiB resources.arsc with a plausible RES_TABLE header.
    arsc_size = 64 * 1024 * 1024
    header = struct.pack("<HHII", 0x0002, 12, arsc_size, 1)  # type, hdrSize, size, pkgCount
    big_arsc = header + b"\x00" * (arsc_size - len(header))
    rewrite_zip(f_png, BAD / "bigarsc.apk", {"resources.arsc": big_arsc})
    del big_arsc
    samples.append({"name": "bigarsc", "file": BAD / "bigarsc.apk",
                    "bytes": (BAD / "bigarsc.apk").stat().st_size,
                    "arscBytes": arsc_size,
                    "description": "f_png.apk with resources.arsc swapped for 64 MiB blob"})

    # f_png.apk with the xxxhdpi icon payload replaced by 4 KiB garbage.
    icon_name = "res/mipmap-xxxhdpi-v4/ic_launcher.png"
    import zipfile
    with zipfile.ZipFile(f_png) as z:
        names = z.namelist()
    if icon_name not in names:
        icon_name = next(n for n in names if n.endswith("xxxhdpi-v4/ic_launcher.png"))
    rewrite_zip(f_png, BAD / "badicon.apk", {icon_name: deterministic_bytes(4096, 0x5A)})
    samples.append({"name": "badicon", "file": BAD / "badicon.apk",
                    "bytes": (BAD / "badicon.apk").stat().st_size,
                    "replacedEntry": icon_name, "entryBytes": 4096,
                    "description": "icon entry data replaced by 4 KiB garbage"})

    # Single entry declaring 300 MiB uncompressed from a real 300 KiB deflate
    # stream (genuinely inflates to 300 MiB -- a true decompression bomb).
    target = 300 * 1024 * 1024
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    parts, crc, chunk = [], 0, b"\x00" * (1 << 20)
    for _ in range(target // len(chunk)):
        parts.append(co.compress(chunk))
        crc = zlib.crc32(chunk, crc)
    parts.append(co.flush())
    stream = b"".join(parts)
    deflate_bomb = build_zip([
        ZipEntry("zeros.bin", stream, method=8, usize=target,
                 crc=crc & 0xFFFFFFFF, precompressed=True),
    ])
    write_bytes(BAD / "deflate_bomb.apk", deflate_bomb)
    samples.append({"name": "deflate_bomb", "file": BAD / "deflate_bomb.apk",
                    "bytes": len(deflate_bomb), "declaredUncompressed": target,
                    "description": "one entry, 300 MiB declared, real high-ratio deflate"})
    return samples


# --------------------------------------------------------------------------
# MANIFEST construction
# --------------------------------------------------------------------------

def find_icon_entries(apk: Path, resource: str) -> list[dict]:
    """Locate <resource> in the APK and classify each variant by qualifier."""
    import zipfile
    base = resource.split("/")[-1]          # ic_launcher
    found = []
    with zipfile.ZipFile(apk) as z:
        for name in z.namelist():
            stem, _, ext = name.rpartition(".")
            if not stem.endswith("/" + base):
                continue
            if not name.startswith("res/"):
                continue
            parts = name.split("/")
            qualifier = parts[1] if len(parts) > 2 else ""
            density = None
            for known in DENSITY_BY_NAME:
                if f"mipmap-{known}" in qualifier and "anydpi" not in qualifier:
                    density = known
            found.append({"pathInApk": name, "qualifier": qualifier,
                          "extension": ext.lower(), "density": density,
                          "bytes": z.getinfo(name).file_size})
    return found


def source_path_for(fixture: str, path_in_apk: str) -> Path:
    """Map an APK entry back to the res/ source file (drops the -v4 suffix)."""
    parts = path_in_apk.split("/")
    qualifier = re.sub(r"-v\d+$", "", parts[1])
    return SRC / fixture / "res" / qualifier / "/".join(parts[2:])


def describe_fixture(spec: dict, apk: Path, aapt2: Path, ffmpeg: str) -> dict:
    entry = {
        "name": spec["name"],
        "apk": apk.relative_to(REPO_ROOT).as_posix(),
        "package": spec["package"],
        "versionCode": spec["versionCode"],
        "versionName": spec["versionName"],
        "minSdk": MIN_SDK,
        "targetSdk": TARGET_SDK,
        "labelDefault": spec["labelDefault"],
        "labelZhCN": spec["labelZhCN"],
        "iconResourceName": spec["iconResourceName"],
        "iconResourceId": None,
        "densities": [],
    }
    if not spec["iconResourceName"]:
        entry["expectedIconPathAt256"] = None
        return entry

    entries = find_icon_entries(apk, spec["iconResourceName"])
    # Resource id (0x7f........) straight out of aapt2's resource dump: the
    # parser under test has to map exactly this id through resources.arsc.
    res_dump = run([aapt2, "dump", "resources", apk], check=False).stdout.decode(
        "utf-8", "replace")
    match = re.search(rf"resource (0x[0-9a-fA-F]+) {re.escape(spec['iconResourceName'])}\s*$",
                      res_dump, re.MULTILINE)
    if match:
        entry["iconResourceId"] = match.group(1)
    import zipfile
    with zipfile.ZipFile(apk) as z:
        for item in entries:
            if item["extension"] not in ("png", "webp"):
                continue
            data = z.read(item["pathInApk"])
            if item["extension"] == "png":
                w, h, pixels = read_png_rgba(data)
                center = rgba_hex(
                    pixels[(h // 2) * w * 4 + (w // 2) * 4:
                           (h // 2) * w * 4 + (w // 2) * 4 + 4])
            else:
                # No stdlib WebP decoder: probe the centre pixel with ffmpeg.
                w = h = DENSITY_BY_NAME.get(item["density"], (0, 0))[1]
                probe = GEN / ".build" / "manifest_probe.webp"
                probe.parent.mkdir(parents=True, exist_ok=True)
                probe.write_bytes(data)
                proc = run([ffmpeg, "-v", "error", "-i", probe, "-vf",
                            f"crop=1:1:{w // 2}:{h // 2}",
                            "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                           check=False)
                center = proc.stdout[:4].hex().upper()
            entry["densities"].append({
                "density": item["density"],
                "densityDpi": {"mdpi": 160, "hdpi": 240, "xhdpi": 320,
                               "xxhdpi": 480, "xxxhdpi": 640}.get(item["density"]),
                "pathInApk": item["pathInApk"],
                "sizePx": [w, h],
                "centerColor": center,
                "bytes": item["bytes"],
            })
    order = {name: i for i, (name, _, _) in enumerate(DENSITIES)}
    rasters = [d for d in entry["densities"] if d["density"]]
    rasters.sort(key=lambda d: order[d["density"]], reverse=True)
    entry["densities"] = sorted(rasters, key=lambda d: order[d["density"]])
    entry["expectedIconPathAt256"] = rasters[0]["pathInApk"] if rasters else None

    xmls = [e for e in entries if e["extension"] == "xml"]
    if xmls:
        adaptive = {"xml": xmls[0]["pathInApk"],
                    "background": f"#{ADAPTIVE_BG_HEX}",
                    "foreground": next(
                        (d["pathInApk"] for d in entry["densities"]
                         if "ic_launcher_fg" in d["pathInApk"]), None),
                    "foregroundColor": f"#{'%02X%02X%02X' % ADAPTIVE_FG}"}
        fg = find_icon_entries(apk, "mipmap/ic_launcher_fg")
        if fg:
            with zipfile.ZipFile(apk) as z:
                w, h, _ = read_png_rgba(z.read(fg[0]["pathInApk"]))
            adaptive["foreground"] = fg[0]["pathInApk"]
            adaptive["foregroundSizePx"] = [w, h]
        entry["adaptive"] = adaptive
    return entry


def badging(apk: Path, aapt2: Path) -> str:
    proc = run([aapt2, "dump", "badging", apk], check=False)
    return proc.stdout.decode("utf-8", "replace")


def collect_real(aapt2: Path) -> list[dict]:
    """Document whatever real-world APKs are present under fixtures/real/."""
    if not REAL.is_dir():
        return []
    out = []
    for apk in sorted(REAL.glob("*.apk")):
        raw = badging(apk, aapt2)
        info = {"name": apk.stem, "apk": apk.relative_to(REPO_ROOT).as_posix(),
                "bytes": apk.stat().st_size}
        for line in raw.splitlines():
            if line.startswith("package:"):
                for key in ("name", "versionCode", "versionName"):
                    marker = f"{key}='"
                    if marker in line:
                        info[key if key != "name" else "package"] = \
                            line.split(marker, 1)[1].split("'", 1)[0]
            elif line.startswith(("sdkVersion:", "minSdkVersion:")):
                info["minSdk"] = line.split(":", 1)[1].strip().strip("'")
            elif line.startswith("targetSdkVersion:"):
                info["targetSdk"] = line.split(":", 1)[1].strip().strip("'")
            elif line.startswith("application-label:"):
                info["label"] = line.split(":", 1)[1].strip().strip("'")
            elif line.startswith("application:"):
                if "icon='" in line:
                    icon = line.split("icon='", 1)[1].split("'", 1)[0]
                    info["declaredIcon"] = icon or None
            elif line.startswith("launchable-activity:"):
                if "name='" in line:
                    info["launchableActivity"] = line.split("name='", 1)[1].split("'", 1)[0]
        try:
            import zipfile
            with zipfile.ZipFile(apk) as z:
                names = z.namelist()
                info["iconEntries"] = [
                    n for n in names
                    if n.startswith("res/") and "mipmap" in n
                    and ("launcher" in n or "/ic_" in n or n.endswith(".webp"))
                ][:12]
                if not info["iconEntries"]:
                    declared = info.get("declaredIcon")
                    if declared:
                        # Obfuscated/shortened resource paths (aapt2 --shorten):
                        # the manifest points at something like res/uF.xml.
                        info["iconEntries"] = [declared] + [
                            n for n in names
                            if n.startswith("res/") and n.endswith(".xml")
                            and n != declared][:11]
                        info["note"] = ("resource paths are obfuscated/shortened "
                                        f"in this APK; manifest icon = {declared}")
                    else:
                        info["iconEntries"] = []
                        info["note"] = ("no android:icon in manifest "
                                        "(aapt2 badging reports icon='')")
        except Exception as exc:  # pragma: no cover - malformed real sample
            info["iconEntriesError"] = str(exc)
        out.append(info)
        print(f"    real: {apk.name} -> {info.get('package')} "
              f"{info.get('versionName')} ({info['bytes']} bytes)")
    return out


# --------------------------------------------------------------------------
# verification
# --------------------------------------------------------------------------

class Checker:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def check(self, cond: bool, msg: str) -> None:
        self.checks += 1
        if not cond:
            self.failures.append(msg)
            print(f"    FAIL  {msg}")

    def eq(self, actual, expected, msg: str) -> None:
        self.check(actual == expected, f"{msg}: expected {expected!r}, got {actual!r}")


def verify(manifest: dict, aapt2: Path, ffmpeg: str) -> Checker:
    """Verify the on-disk MANIFEST.json against the on-disk artifacts.

    manifest is the JSON re-read from MANIFEST_PATH, so a wrong entry path in
    the published file fails here rather than silently matching memory.
    """
    import zipfile
    ck = Checker()

    for fixture in manifest["fixtures"]:
        name = fixture["name"]
        apk = REPO_ROOT / fixture["apk"]
        ck.check(apk.is_file(), f"{name}: MANIFEST apk path missing: {fixture['apk']}")
        if not apk.is_file():
            continue
        with zipfile.ZipFile(apk) as z:
            names = set(z.namelist())
            ck.check("AndroidManifest.xml" in names, f"{name}: no AndroidManifest.xml")
            ck.check("resources.arsc" in names, f"{name}: no resources.arsc")
            manifest_bytes = z.read("AndroidManifest.xml")
            ck.eq(manifest_bytes[:4].hex(), "03000800",
                  f"{name}: AXML magic")
            arsc = z.read("resources.arsc")
            ck.eq(arsc[:4].hex(), "02000c00", f"{name}: RES_TABLE magic")
            ck.check(len(arsc) < 1 << 20, f"{name}: resources.arsc unexpectedly large")

            for dens in fixture["densities"]:
                path = dens["pathInApk"]
                ck.check(path in names, f"{name}: missing entry {path}")
                if path not in names:
                    continue
                payload = z.read(path)
                if dens["pathInApk"].endswith(".png"):
                    w, h, pixels = read_png_rgba(payload)
                    ck.eq([w, h], dens["sizePx"], f"{name}: {path} dimensions")
                    ck.eq(rgba_hex(pixels[(h // 2) * w * 4 + (w // 2) * 4:
                                           (h // 2) * w * 4 + (w // 2) * 4 + 4]),
                          dens["centerColor"], f"{name}: {path} centre colour")
                    corners = {rgba_hex(pixels[0:4]),
                               rgba_hex(pixels[(w - 1) * 4:(w - 1) * 4 + 4]),
                               rgba_hex(pixels[((h - 1) * w) * 4:((h - 1) * w) * 4 + 4])}
                    ck.check(len(corners) == 1 and dens["centerColor"] in corners,
                             f"{name}: {path} is not a single flat colour")
                    src = source_path_for(name, path)
                    ck.check(src.is_file() and src.read_bytes() == payload,
                             f"{name}: {path} was repacked by aapt2")
                else:
                    # WebP: prove aapt2 did not transcode it (byte identity),
                    # then decode the centre pixel with ffmpeg.
                    src = source_path_for(name, path)
                    ck.check(src.is_file() and src.read_bytes() == payload,
                             f"{name}: {path} was repacked by aapt2")
                    probe = GEN / ".build" / "webp_probe.webp"
                    probe.parent.mkdir(parents=True, exist_ok=True)
                    probe.write_bytes(payload)
                    proc = run([ffmpeg, "-v", "error", "-i", probe,
                                "-vf", f"crop=1:1:{dens['sizePx'][0] // 2}:"
                                       f"{dens['sizePx'][1] // 2}",
                                "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                               check=False)
                    ck.eq(proc.stdout[:4].hex().upper(), dens["centerColor"],
                          f"{name}: {path} centre colour (ffmpeg decode)")

        raw = badging(apk, aapt2)
        if fixture["iconResourceName"]:
            ck.check(bool(fixture["iconResourceId"])
                     and fixture["iconResourceId"].startswith("0x7f"),
                     f"{name}: icon resource id not resolved from dump resources")
        ck.check(f"package: name='{fixture['package']}'" in raw,
                 f"{name}: badging package")
        ck.check(f"versionCode='{fixture['versionCode']}'" in raw,
                 f"{name}: badging versionCode")
        ck.check(f"versionName='{fixture['versionName']}'" in raw,
                 f"{name}: badging versionName")
        # aapt2 >= 2.19 prints minSdkVersion:'N'; older builds print sdkVersion:'N'.
        ck.check(f"minSdkVersion:'{MIN_SDK}'" in raw or f"sdkVersion:'{MIN_SDK}'" in raw,
                 f"{name}: badging minSdk")
        ck.check(f"targetSdkVersion:'{TARGET_SDK}'" in raw, f"{name}: badging targetSdk")
        ck.check("launchable-activity:" in raw, f"{name}: badging launchable activity")
        ck.check(f"application-label:'{fixture['labelDefault']}'" in raw,
                 f"{name}: badging default label")
        if fixture["labelZhCN"]:
            ck.check(f"application-label-zh-CN:'{fixture['labelZhCN']}'" in raw,
                     f"{name}: badging zh-rCN label")
        if fixture["iconResourceName"] is None:
            ck.check(f"application: label='{fixture['labelDefault']}' icon=''" in raw,
                     f"{name}: unexpected application icon in badging")
        else:
            # aapt2 badging reports every density variant as 'application-icon-N:'.
            # For adaptive icons it resolves anydpi-v26 for *all* densities, so
            # the raster fallbacks never show up there -- their presence is
            # asserted from the ZIP above instead.
            icon_lines = [ln for ln in raw.splitlines()
                          if ln.startswith("application-icon-")]
            if fixture.get("adaptive"):
                ck.check(any(fixture["adaptive"]["xml"] in ln for ln in icon_lines),
                         f"{name}: badging has no adaptive-icon variant")
                ck.check(not any("ic_launcher.png" in ln for ln in icon_lines),
                         f"{name}: raster fallback unexpectedly shadowed the "
                         f"adaptive icon in badging")
            else:
                for dens in fixture["densities"]:
                    ck.check(any(dens["pathInApk"] in ln for ln in icon_lines),
                             f"{name}: badging has no application-icon for "
                             f"{dens['pathInApk']}")

    # ---- adversarial samples (driven by the published MANIFEST entries) ----
    bad_index = {b["name"]: b for b in manifest["bad"]}
    ck.eq(sorted(bad_index), sorted(
        ["empty", "trunc", "notzip", "bomb_entries", "traversal", "bigarsc",
         "badicon", "deflate_bomb"]), "bad: MANIFEST sample list")
    for item in manifest["bad"]:
        path = REPO_ROOT / item["file"]
        ck.check(path.is_file(), f"bad: {item['name']} missing at {item['file']}")
        if path.is_file():
            ck.eq(path.stat().st_size, item["bytes"], f"bad: {item['name']} size")

    ck.eq((BAD / "empty.apk").stat().st_size, 0, "bad: empty.apk size")
    f_png_size = (GEN / "f_png.apk").stat().st_size
    ck.eq(bad_index["trunc"].get("truncatedFrom"), f_png_size,
          "bad: trunc truncatedFrom vs f_png size")
    ck.eq((BAD / "trunc.apk").stat().st_size, int(f_png_size * 0.6),
          "bad: trunc.apk size (60% of f_png)")
    ck.eq((BAD / "notzip.apk").stat().st_size, 4096, "bad: notzip.apk size")
    ck.check((BAD / "notzip.apk").read_bytes()[:2] != b"PK",
             "bad: notzip.apk starts with PK")
    ck.eq(zip_count((BAD / "bomb_entries.apk").read_bytes()), 70000,
          "bad: bomb_entries.apk entry count")
    with zipfile.ZipFile(BAD / "bomb_entries.apk") as z:
        infos = z.infolist()
        ck.eq(len(infos), 70000, "bad: bomb_entries namelist length")
        ck.eq(bad_index["bomb_entries"]["entryCount"], 70000,
              "bad: MANIFEST bomb_entries entryCount")
        ck.check(all(i.compress_type == 0 for i in infos),
                 "bad: bomb_entries not all stored")
        ck.check(all(i.file_size == 0 for i in infos),
                 "bad: bomb_entries not all zero-byte")
    with zipfile.ZipFile(BAD / "traversal.apk") as z:
        names = z.namelist()
        ck.check("../../evil.dll" in names, "bad: traversal missing ../../evil.dll")
        ck.check("res/mipmap-xxxhdpi/ic_launcher.png" in names,
                 "bad: traversal missing icon entry")
        try:
            z.read("res/mipmap-xxxhdpi/ic_launcher.png")
            ck.check(False, "bad: traversal icon entry unexpectedly readable")
        except Exception:
            ck.check(True, "")
    with zipfile.ZipFile(BAD / "bigarsc.apk") as z:
        arsc = z.getinfo("resources.arsc")
        ck.eq(arsc.file_size, 64 * 1024 * 1024, "bad: bigarsc resources.arsc size")
        head = z.open("resources.arsc").read(12)
        ck.eq(head[:4].hex(), "02000c00", "bad: bigarsc RES_TABLE magic")
    with zipfile.ZipFile(BAD / "badicon.apk") as z:
        ck.eq(z.getinfo(bad_index["badicon"]["replacedEntry"]).file_size, 4096,
              "bad: badicon replaced entry size")
        ck.check(z.read("AndroidManifest.xml")[:4].hex() == "03000800",
                 "bad: badicon manifest no longer AXML")
    with zipfile.ZipFile(BAD / "deflate_bomb.apk") as z:
        infos = z.infolist()
        ck.eq(len(infos), 1, "bad: deflate_bomb entry count")
        ck.eq(infos[0].file_size, 300 * 1024 * 1024,
              "bad: deflate_bomb declared uncompressed size")
        ck.check(infos[0].compress_size < 1 << 20,
                 "bad: deflate_bomb compressed size not small")
        ck.check(infos[0].file_size // max(infos[0].compress_size, 1) > 1000,
                 "bad: deflate_bomb ratio below 1000:1")
    return ck


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    aapt2, android_jar, ffmpeg = tool_paths()
    print(f"repo      : {REPO_ROOT}")
    print(f"aapt2     : {aapt2}")
    print(f"android.jar: {android_jar}")
    print(f"ffmpeg    : {ffmpeg}")

    if not aapt2.is_file():
        raise SystemExit(f"aapt2 not found: {aapt2} (set AAPT2=...)")
    if not android_jar.is_file():
        raise SystemExit(f"android.jar not found: {android_jar} (set ANDROID_JAR=...)")
    encoders = run([ffmpeg, "-hide_banner", "-encoders"], check=False)
    if b"libwebp" not in encoders.stdout:
        raise SystemExit("ffmpeg has no libwebp encoder")

    print("\n[1/5] cleaning generated/")
    if GEN.exists():
        shutil.rmtree(GEN)
    GEN.mkdir(parents=True)

    print("[2/5] writing sources + compiling APKs")
    specs = [
        build_f_png(),
        build_f_webp(ffmpeg),
        build_f_noicon(),
        build_f_adaptive(),
    ]
    apks: dict[str, Path] = {}
    for spec in specs:
        print(f"  - {spec['name']}")
        apks[spec["name"]] = compile_and_link(spec, aapt2, android_jar)

    print("[3/5] building adversarial samples")
    BAD.mkdir(parents=True, exist_ok=True)
    bad = build_bad()

    print("[4/5] writing MANIFEST.json")
    fixtures = [describe_fixture(spec, apks[spec["name"]], aapt2, ffmpeg)
                for spec in specs]
    real = collect_real(aapt2)
    manifest = {
        "generatedBy": "tests/gen_fixtures.py",
        "note": ("APKs under generated/ are produced by aapt2 from "
                 "tests/fixtures/src/*; do not edit by hand. "
                 "'expectedIconPathAt256' is the entry a 256px request should "
                 "pick (highest-density real raster; adaptive XML is not a "
                 "raster and ranks below)."),
        "aapt2": str(aapt2),
        "androidJar": str(android_jar),
        "minSdk": MIN_SDK,
        "targetSdk": TARGET_SDK,
        "densityColorLegend": {k: "#" + v for k, v in PNG_HEX.items()},
        "expectedIconPathAt256": {
            f["name"]: f["expectedIconPathAt256"] for f in fixtures
        },
        "fixtures": fixtures,
        "bad": [{k: (v.relative_to(REPO_ROOT).as_posix() if k == "file" else v)
                 for k, v in b.items()} for b in bad],
        "real": real,
    }
    with open(MANIFEST_PATH, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, ensure_ascii=False, indent=2, sort_keys=False)
        fh.write("\n")

    print("[5/5] verifying")
    with open(MANIFEST_PATH, encoding="utf-8") as fh:
        published = json.load(fh)   # verify the file, not our in-memory copy
    ck = verify(published, aapt2, ffmpeg)
    if ck.failures:
        for failure in ck.failures:
            print(f"  FAILED: {failure}")
        print(f"\n{len(ck.failures)} of {ck.checks} checks FAILED")
        return 1

    build_dir = GEN / ".build"
    if build_dir.exists() and "--keep-build" not in sys.argv:
        shutil.rmtree(build_dir)

    print("\n=== fixtures ===")
    header = f"{'fixture':<11} {'apk':<14} {'package':<26} {'ver':<8} {'size':>9}  icon@256"
    print(header)
    print("-" * len(header))
    for f in fixtures:
        size = apks[f["name"]].stat().st_size
        print(f"{f['name']:<11} {f['name'] + '.apk':<14} {f['package']:<26} "
              f"{f['versionName']:<8} {size:>9}  {f['expectedIconPathAt256']}")
    total = sum(apks[n].stat().st_size for n in apks)
    print(f"{'total':<11} {'':<14} {'':<26} {'':<8} {total:>9}")
    print("\n=== bad samples ===")
    for b in bad:
        print(f"{b['name']:<15} {b['bytes']:>10} bytes  {b['description']}")
    print(f"\nMANIFEST: {MANIFEST_PATH}")
    print(f"{ck.checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
