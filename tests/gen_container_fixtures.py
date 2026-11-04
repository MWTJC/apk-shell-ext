#!/usr/bin/env python3
"""Bad-package fixtures for the multi-format paths (M4a containers, M4d HAP).

Everything here is built from scratch, so it is reproducible without any real sample:

  bad/xapk_no_apk.xapk          outer ZIP with no AndroidManifest.xml and no *.apk
  bad/xapk_broken_base.xapk     has base.apk, but the entry is 128 bytes of garbage
  bad/xapk_bomb_base.xapk       base.apk is a tiny deflate entry that claims to be an APK
  bad/xapk_truncated.xapk       copy of a real .xapk cut to its first 1 MiB (needs the sample)
  bad/hap_no_bundle.hap         module.json parses but has no bundleName
  bad/hap_bad_json.hap          module.json is not valid JSON
  bad/hap_no_icon.hap           valid HAP metadata, no resources/media at all

Usage:
    python tests/gen_container_fixtures.py [--real <file.xapk>]

Exit code 0 = fixtures written (prints their paths).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tests" / "fixtures" / "generated" / "bad"


def write_zip(path: Path, entries: dict[str, tuple[bytes, int]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as z:
        for name, (data, method) in entries.items():
            z.writestr(zipfile.ZipInfo(name), data, compress_type=method)


def hap_module_json(bundle_name: str = "com.example.hap") -> bytes:
    module = {
        "app": {
            "bundleName": bundle_name,
            "versionName": "1.0.0",
            "versionCode": 1000000,
            "minAPIVersion": 60000020,
            "targetAPIVersion": 60000020,
            "compileSdkVersion": "6.0.0.47",
            "icon": "$media:app_icon",
            "label": "Example HAP",
        },
        "module": {
            "name": "entry",
            "type": "entry",
            "mainElement": "EntryAbility",
            "deviceTypes": ["phone"],
            "abilities": [{"name": "EntryAbility", "startWindowIcon": "$media:startIcon"}],
        },
    }
    return json.dumps(module, ensure_ascii=False).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--real", default="", help="optional real .xapk to truncate")
    args = parser.parse_args()

    # ---------------------------------------------------------------- 多 APK 容器
    write_zip(OUT / "xapk_no_apk.xapk", {"readme.txt": (b"no apk here\n", zipfile.ZIP_STORED)})
    write_zip(
        OUT / "xapk_broken_base.xapk",
        {
            "base.apk": (b"PK\x03\x04" + b"\x00" * 124, zipfile.ZIP_STORED),
            "manifest.json": (b'{"package_name":"com.example.broken"}', zipfile.ZIP_DEFLATED),
        },
    )
    # 声明成 deflate 的小条目：必须被 ZIP 结构/限额挡住而不是无限解压
    write_zip(
        OUT / "xapk_bomb_base.xapk",
        {
            "base.apk": (b"\x00" * 4096, zipfile.ZIP_DEFLATED),
            "manifest.json": (
                b'{"package_name":"com.example.bomb","version_code":"1"}',
                zipfile.ZIP_DEFLATED,
            ),
        },
    )

    # ---------------------------------------------------------------- 鸿蒙 HAP
    write_zip(
        OUT / "hap_no_bundle.hap",
        {
            "module.json": (hap_module_json(""), zipfile.ZIP_STORED),
            "resources/base/media/app_icon.png": (b"\x89PNG\r\n\x1a\n", zipfile.ZIP_STORED),
        },
    )
    write_zip(
        OUT / "hap_bad_json.hap",
        {"module.json": (b'{"app": {"bundleName": "com.example', zipfile.ZIP_STORED)},
    )
    write_zip(
        OUT / "hap_no_icon.hap",
        {
            "module.json": (hap_module_json(), zipfile.ZIP_STORED),
            "pack.info": (b'{"summary":{}}', zipfile.ZIP_STORED),
        },
    )

    if args.real:
        src = Path(args.real)
        if not src.exists():
            print(f"real sample not found: {src}", file=sys.stderr)
            return 2
        with src.open("rb") as f:
            head = f.read(1024 * 1024)
        target = OUT / "xapk_truncated.xapk"
        target.write_bytes(head)
        print(target)

    for name in sorted(os.listdir(OUT)):
        print(OUT / name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
