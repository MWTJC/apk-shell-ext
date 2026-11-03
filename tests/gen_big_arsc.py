#!/usr/bin/env python3
"""Generate a real APK whose resources.arsc is larger than the 8 MB TD-001 cap.

Why: the TD-001 debt says "M1 reads resources.arsc whole (<= 8 MB), M2 must switch to
chunked streaming and delete that path". A 64 MB *blob* is not a usable fixture for that
(it has no parseable chunks), so this script builds a genuine table with aapt2:
many long strings x several locales, which grows the string pool past 8 MB while keeping
~40 resource entries.

Also emits tests/fixtures/generated/tdeflate_arsc.apk: f_png.apk with resources.arsc
re-packaged as Deflated, i.e. the one case the streaming reader cannot serve (TD-002).

Usage: python tests/gen_big_arsc.py [--strings 4000] [--chars 1200] [--locales zh-rCN,ja,de]
Output: tests/fixtures/generated/f_bigarsc_real.apk + tdeflate_arsc.apk
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "tests" / "fixtures" / "generated"
SRC = ROOT / "tests" / "fixtures" / "src" / "f_bigarsc_real"
AAPT2 = os.environ.get("AAPT2", "E:/PROGRAMS/Android/Sdk/build-tools/36.0.0/aapt2.exe")
ANDROID_JAR = os.environ.get(
    "ANDROID_JAR", "E:/PROGRAMS/Android/Sdk/platforms/android-36/android.jar"
)

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.example.bigarsc" android:versionCode="7001" android:versionName="7.0.1">
  <uses-sdk android:minSdkVersion="26" android:targetSdkVersion="34" />
  <application android:label="@string/app_name" android:icon="@mipmap/ic_launcher">
    <activity android:name=".MainActivity" android:exported="true">
      <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
      </intent-filter>
    </activity>
  </application>
</manifest>
"""


def solid_png(size: int, rgb: tuple[int, int, int]) -> bytes:
    import struct
    import zlib

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    row = b"\x00" + bytes(rgb + (255,)) * size
    raw = row * size
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def write_strings(path: Path, count: int, chars: int, heading: str) -> None:
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<resources>"]
    lines.append(f'  <string name="app_name">{heading}</string>')
    filler = ("文" * chars) if "zh" in heading.lower() else ("x" * chars)
    for i in range(count):
        # 每条都不同，避免 aapt2 做字符串去重（去重会缩小表）
        lines.append(f'  <string name="blob_{i:05d}">{i}-{filler}</string>')
    lines.append("</resources>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strings", type=int, default=4000)
    ap.add_argument("--chars", type=int, default=1200)
    ap.add_argument("--locales", default="zh-rCN,ja,de")
    ap.add_argument("--min-bytes", type=int, default=8 * 1024 * 1024)
    args = ap.parse_args()

    if SRC.exists():
        shutil.rmtree(SRC)
    (SRC / "res" / "values").mkdir(parents=True)
    (SRC / "AndroidManifest.xml").write_text(MANIFEST, encoding="utf-8")
    write_strings(SRC / "res" / "values" / "strings.xml", args.strings, args.chars, "BigArsc")
    for loc in [x.strip() for x in args.locales.split(",") if x.strip()]:
        write_strings(SRC / "res" / f"values-{loc}" / "strings.xml", args.strings, args.chars,
                      f"BigArsc-{loc}")
    icon_dir = SRC / "res" / "mipmap-xxxhdpi"
    icon_dir.mkdir(parents=True, exist_ok=True)
    (icon_dir / "ic_launcher.png").write_bytes(solid_png(192, (0x33, 0x99, 0xCC)))

    GEN.mkdir(parents=True, exist_ok=True)
    build = GEN / ".build" / "f_bigarsc_real"
    build.mkdir(parents=True, exist_ok=True)
    compiled = build / "compiled.zip"
    out_apk = GEN / "f_bigarsc_real.apk"

    subprocess.run([AAPT2, "compile", "--no-crunch", "--dir", str(SRC / "res"), "-o",
                    str(compiled)], check=True)
    subprocess.run([AAPT2, "link", "-o", str(out_apk), "--manifest",
                    str(SRC / "AndroidManifest.xml"), "-I", ANDROID_JAR,
                    "--min-sdk-version", "26", "--target-sdk-version", "34",
                    "--auto-add-overlay", str(compiled)], check=True)

    with zipfile.ZipFile(out_apk) as zf:
        info = zf.getinfo("resources.arsc")
        method = info.compress_type
    size = info.file_size if hasattr(info, "file_size") else 0
    method_name = {0: "Stored", 8: "Deflated"}.get(method, str(method))
    print(f"f_bigarsc_real.apk: resources.arsc = {size:,} bytes ({method_name}), "
          f"apk = {out_apk.stat().st_size:,} bytes")
    if size < args.min_bytes:
        print(f"ERROR: arsc {size:,} < requested minimum {args.min_bytes:,}; "
              f"raise --strings/--chars", file=sys.stderr)
        return 1

    # TD-002 sample: the same small table, but Deflated (streaming cannot seek a deflate stream)
    src_png = GEN / "f_png.apk"
    if src_png.exists():
        out_deflate = GEN / "tdeflate_arsc.apk"
        with zipfile.ZipFile(src_png) as zin, zipfile.ZipFile(
            out_deflate, "w", zipfile.ZIP_DEFLATED, compresslevel=9
        ) as zout:
            for item in zin.infolist():
                data = zin.read(item.filename)
                zi = zipfile.ZipInfo(item.filename, date_time=item.date_time)
                zi.compress_type = (
                    zipfile.ZIP_DEFLATED if item.filename == "resources.arsc" else item.compress_type
                )
                zout.writestr(zi, data)
        with zipfile.ZipFile(out_deflate) as zf:
            d = zf.getinfo("resources.arsc")
            print(f"tdeflate_arsc.apk: resources.arsc = {d.file_size:,} bytes (Deflated)")
    else:
        print("skip tdeflate_arsc.apk (run gen_fixtures.py first)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
