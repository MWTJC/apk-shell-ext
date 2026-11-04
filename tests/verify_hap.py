#!/usr/bin/env python3
"""M4d acceptance: cross-check apkinfo.exe on HarmonyOS .hap against module.json.

There is no official CLI that dumps a HAP (hvigor lives in DevEco), so the reference
here is the HAP's own `module.json`, parsed independently in Python:

  * bundleName / versionName / versionCode / min-targetAPIVersion / module / type /
    mainAbility / deviceTypes / abilities / requestPermissions
  * the reported icon entry must exist in the ZIP and be a real image (PNG/WebP/JPEG
    magic bytes), and its pixel size must match icon.width/height when PNG/WebP

Usage:
    python tests/verify_hap.py [--apkinfo PATH] <hap> [<hap> ...]
    python tests/verify_hap.py "tests/fixtures/real/*.hap"

Exit code 0 = matched, 1 = differences, 2 = harness error.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_APKINFO = os.environ.get("APKINFO", str(ROOT / "out" / "x64" / "apkinfo.exe"))


def image_size(data: bytes) -> tuple[str, int, int] | None:
    if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) >= 24:
        w, h = struct.unpack(">II", data[16:24])
        return ("png", w, h)
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        # VP8X / VP8 / VP8L：只认 VP8X（带扩展头）的 24 位宽高
        chunk = data[12:16]
        if chunk == b"VP8X" and len(data) >= 30:
            w = int.from_bytes(data[24:27], "little") + 1
            h = int.from_bytes(data[27:30], "little") + 1
            return ("webp", w, h)
        if chunk == b"VP8 " and len(data) >= 30:
            w = int.from_bytes(data[26:28], "little") & 0x3FFF
            h = int.from_bytes(data[28:30], "little") & 0x3FFF
            return ("webp", w, h)
        return ("webp", 0, 0)
    if data[:3] == b"\xff\xd8\xff":
        return ("jpeg", 0, 0)
    return None


def parse_string_resources(index_bytes: bytes) -> dict[str, str]:
    """Independent RestoolV2 reader: string resource name -> default-config value.

    Layout mirrors openharmony/developtools_global_resource_tool's resource_table.cpp:
    IndexHeaderV2 (version[128] + length + keyCount + dataBlockOffset), KeyConfig blocks,
    an IDSS block of ResTypeHeader/ResIndex, and ResInfo/value tables addressed by offset.
    """
    u32 = lambda o: struct.unpack_from("<I", index_bytes, o)[0]
    u16 = lambda o: struct.unpack_from("<H", index_bytes, o)[0]

    key_count = u32(0x84)
    cursor = 140
    defaults: list[int] = []
    for _ in range(key_count):
        config_id, param_count = u32(cursor + 4), u32(cursor + 8)
        cursor += 12 + param_count * 8
        if param_count == 0:
            defaults.append(config_id)

    if index_bytes[cursor:cursor + 4] != b"IDSS":
        raise RuntimeError("not a RestoolV2 index (no IDSS tag)")

    type_count = u32(cursor + 8)
    cursor += 16
    out: dict[str, str] = {}
    for _ in range(type_count):
        res_type, _length, count = u32(cursor), u32(cursor + 4), u32(cursor + 8)
        cursor += 12
        for _ in range(count):
            res_id, offset, name_len = u32(cursor), u32(cursor + 4), u32(cursor + 8)
            name = index_bytes[cursor + 12:cursor + 12 + name_len].decode("utf-8", "replace")
            cursor += 12 + name_len
            if res_type != 9:  # 9 = string
                continue
            value_count = u32(offset + 8)
            pos = offset + 12
            chosen = None
            for _ in range(value_count):
                config_id, data_offset = u32(pos), u32(pos + 4)
                pos += 8
                length = u16(data_offset)
                value = index_bytes[data_offset + 2:data_offset + 2 + length]
                if config_id in defaults:
                    chosen = value
            if chosen is not None:
                out[name] = chosen.decode("utf-8", "replace")
    return out


def expected_label(module_json: dict, index_bytes: bytes) -> dict[str, str]:
    """Home screen shows the UIAbility label; app.label is only the fallback."""
    app = module_json.get("app", {})
    module = module_json.get("module", {})
    main = module.get("mainElement", "")
    ref = ""
    for ability in module.get("abilities") or []:
        if main and ability.get("name") == main:
            ref = ability.get("label", "")
            break
    if not ref:
        ref = app.get("label", "")
    if not ref.startswith("$string:"):
        return {"ref": ref, "value": ref}
    value = parse_string_resources(index_bytes).get(ref[len("$string:"):], "")
    return {"ref": ref, "value": value}


def expected_from_manifest(module_json: dict) -> dict:
    app = module_json.get("app", {})
    module = module_json.get("module", {})
    return {
        "package": app.get("bundleName", ""),
        "versionName": app.get("versionName", ""),
        "versionCode": app.get("versionCode", 0),
        "minAPI": app.get("minAPIVersion", 0),
        "targetAPI": app.get("targetAPIVersion", 0),
        "compileSdk": app.get("compileSdkVersion", ""),
        "module": module.get("name", ""),
        "type": module.get("type", ""),
        "mainAbility": module.get("mainElement", ""),
        "deviceTypes": module.get("deviceTypes", []),
        "abilities": len(module.get("abilities", []) or []),
        "extensionAbilities": len(module.get("extensionAbilities", []) or []),
        "permissions": [p.get("name") for p in (module.get("requestPermissions") or [])
                        if p.get("name")],
    }


def check_one(apkinfo: str, hap: Path) -> list[str]:
    problems: list[str] = []
    out = subprocess.run([apkinfo, "--json", str(hap)], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    if out.returncode != 0:
        return [f"{hap.name}: apkinfo exit={out.returncode} {out.stderr.strip()[:160]}"]
    info = json.loads(out.stdout)

    if info.get("kind") != "hap":
        problems.append(f"{hap.name}: kind={info.get('kind')!r} (expected 'hap')")

    with zipfile.ZipFile(hap) as z:
        module_json = json.loads(z.read("module.json"))
        expected = expected_from_manifest(module_json)
        label = expected_label(module_json, z.read("resources.index"))
        ours_label = info.get("app", {}).get("label", "")
        if label["value"] and ours_label != label["value"]:
            problems.append(
                f"{hap.name}: label ours={ours_label!r} index={label['value']!r} ({label['ref']})"
            )

        app = info.get("app", {})
        hap_block = info.get("hap", {})
        for key in ("package", "versionName", "versionCode"):
            if app.get(key) != expected[key]:
                problems.append(f"{hap.name}: app.{key} ours={app.get(key)!r} manifest={expected[key]!r}")
        for key in ("minAPI", "targetAPI", "compileSdk", "module", "type", "mainAbility"):
            if hap_block.get(key) != expected[key]:
                problems.append(f"{hap.name}: hap.{key} ours={hap_block.get(key)!r} manifest={expected[key]!r}")
        if sorted(hap_block.get("deviceTypes", [])) != sorted(expected["deviceTypes"]):
            problems.append(f"{hap.name}: deviceTypes {hap_block.get('deviceTypes')} != {expected['deviceTypes']}")
        if hap_block.get("abilities") != expected["abilities"]:
            problems.append(f"{hap.name}: abilities {hap_block.get('abilities')} != {expected['abilities']}")
        if hap_block.get("extensionAbilities", 0) != expected["extensionAbilities"]:
            problems.append(f"{hap.name}: extensionAbilities {hap_block.get('extensionAbilities')} != {expected['extensionAbilities']}")

        ours = sorted(p.get("name") for p in info.get("permissions", []))
        if ours != sorted(expected["permissions"]):
            problems.append(f"{hap.name}: permissions ours={ours} manifest={expected['permissions']}")

        icon = info.get("icon") or {}
        source = icon.get("source", "")
        if not source:
            problems.append(f"{hap.name}: no icon reported")
        else:
            if source not in z.namelist():
                problems.append(f"{hap.name}: icon {source!r} not in ZIP")
            else:
                data = z.read(source)
                size = image_size(data)
                if size is None:
                    problems.append(f"{hap.name}: icon {source!r} is not PNG/WebP/JPEG")
                else:
                    kind, w, h = size
                    if w and h and (icon.get("width") != w or icon.get("height") != h):
                        problems.append(
                            f"{hap.name}: icon size ours={icon.get('width')}x{icon.get('height')} "
                            f"file={w}x{h}"
                        )
                    print(f"  {hap.name}: {expected['package']} {expected['versionName']} "
                          f"label={ours_label!r} "
                          f"({expected['versionCode']}) module={expected['module']} "
                          f"icon={source} [{kind} {w}x{h}] "
                          f"{info.get('diagnostics', {}).get('elapsedMs')} ms")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apkinfo", default=DEFAULT_APKINFO)
    parser.add_argument("packages", nargs="+")
    args = parser.parse_args()

    if not Path(args.apkinfo).exists():
        print(f"apkinfo not found: {args.apkinfo}", file=sys.stderr)
        return 2

    targets: list[Path] = []
    for pattern in args.packages:
        matched = [Path(p) for p in glob.glob(pattern)]
        targets.extend(matched if matched else [Path(pattern)])
    targets = [p for p in targets if p.exists()]
    if not targets:
        print("no .hap matched", file=sys.stderr)
        return 2

    problems: list[str] = []
    matched = 0
    for hap in targets:
        found = check_one(args.apkinfo, hap)
        if found:
            problems.extend(found)
        else:
            matched += 1

    print()
    print(f"{matched}/{len(targets)} HAP matched module.json cross-check")
    for problem in problems:
        print(f"  FAIL {problem}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
