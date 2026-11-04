#!/usr/bin/env python3
"""M4a acceptance: cross-check multi-APK containers (.xapk/.apks/.apkm) against aapt2.

For every container on the command line this checks three things (each independently):

  1. container detection  : apkinfo.exe reports kind = xapk/apks/apkm/splits (not "apk")
  2. base selection       : the reported base entry exists in the outer ZIP, and its
                            uncompressed size matches the central directory
  3. field correctness    : package / versionCode / versionName / minSdk / targetSdk of
                            the *base* APK match `aapt2 dump badging` on the extracted base
  4. split listing        : every .apk at the outer root appears exactly once in
                            container.splits, and the icon entry exists inside the base

Usage:
    python tests/verify_container.py [--aapt2 PATH] [--apkinfo PATH] <pkg> [<pkg> ...]
    python tests/verify_container.py "tests/fixtures/real/*.xapk"

Exit code 0 = everything matched, 1 = differences, 2 = harness error.
The container itself is never written anywhere except a temp copy of the base APK
(needed because aapt2 only takes a path).
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_AAPT2 = os.environ.get("AAPT2", "E:/PROGRAMS/Android/Sdk/build-tools/36.0.0/aapt2.exe")
DEFAULT_APKINFO = os.environ.get("APKINFO", str(ROOT / "out" / "x64" / "apkinfo.exe"))


def run_apkinfo(apkinfo: str, pkg: Path) -> dict | None:
    out = subprocess.run(
        [apkinfo, "--json", str(pkg)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if out.returncode != 0:
        print(f"  apkinfo exit={out.returncode} stderr={out.stderr.strip()[:200]}")
        return None
    return json.loads(out.stdout)


def aapt2_badging(aapt2: str, base_apk: Path) -> dict:
    out = subprocess.run(
        [aapt2, "dump", "badging", str(base_apk)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if out.returncode != 0:
        raise RuntimeError(f"aapt2 failed: {out.stderr[:300]}")
    text = out.stdout
    data: dict = {}
    m = re.search(r"package: name='([^']*)' versionCode='([^']*)' versionName='([^']*)'", text)
    if m:
        data["package"] = m.group(1)
        data["versionCode"] = int(m.group(2) or 0)
        data["versionName"] = m.group(3)
    m = re.search(r"(?:sdkVersion|minSdkVersion):'(\d+)'", text)
    if m:
        data["minSdk"] = int(m.group(1))
    m = re.search(r"targetSdkVersion:'(\d+)'", text)
    if m:
        data["targetSdk"] = int(m.group(1))
    return data


def check_one(aapt2: str, apkinfo: str, pkg: Path) -> list[str]:
    problems: list[str] = []
    info = run_apkinfo(apkinfo, pkg)
    if info is None:
        return [f"{pkg.name}: apkinfo failed"]

    kind = info.get("kind")
    if kind not in ("xapk", "apks", "apkm", "splits"):
        problems.append(f"{pkg.name}: kind={kind!r} (expected a container kind)")

    container = info.get("container")
    if not container:
        return problems + [f"{pkg.name}: JSON has no container block"]

    base_name = container.get("base", "")
    splits = container.get("splits", [])

    with zipfile.ZipFile(pkg) as z:
        names = {i.filename: i for i in z.infolist()}
        root_apks = sorted(
            n for n in names if "/" not in n and n.lower().endswith(".apk")
        )
        if base_name not in names:
            problems.append(f"{pkg.name}: base {base_name!r} not in ZIP")
            return problems
        if names[base_name].file_size != next(
            (s["size"] for s in splits if s.get("base")), -1
        ):
            problems.append(f"{pkg.name}: base size mismatch between ZIP and JSON")

        listed = [s["name"] for s in splits]
        if sorted(listed) != root_apks:
            problems.append(
                f"{pkg.name}: splits {listed} != root .apk {root_apks}"
            )
        if sum(1 for s in splits if s.get("base")) != 1:
            problems.append(f"{pkg.name}: expected exactly one split flagged as base")

        icon = info.get("icon") or {}
        icon_entry = icon.get("source", "")
        icon_from_root = icon.get("fromContainer", False)
        if icon_entry and not icon_from_root:
            inner = icon_entry.split("!", 1)[-1]
            with z.open(base_name) as raw, tempfile.TemporaryFile() as tmp:
                tmp.write(raw.read())
                tmp.seek(0)
                with zipfile.ZipFile(tmp) as base_zip:
                    if inner not in base_zip.namelist():
                        problems.append(f"{pkg.name}: icon {inner!r} not in base APK")

        # aapt2 对照（需要把 base 解出来落到临时文件）
        with z.open(base_name) as raw, tempfile.NamedTemporaryFile(
            suffix=".apk", delete=False
        ) as tmp:
            tmp.write(raw.read())
            tmp_path = Path(tmp.name)
        try:
            expected = aapt2_badging(aapt2, tmp_path)
        finally:
            tmp_path.unlink(missing_ok=True)

    app = info.get("app", {})
    for key in ("package", "versionCode", "versionName", "minSdk", "targetSdk"):
        if key not in expected:
            continue
        if app.get(key) != expected[key]:
            problems.append(
                f"{pkg.name}: {key} ours={app.get(key)!r} aapt2={expected[key]!r}"
            )

    icon_note = icon.get("source", "-")
    print(
        f"  {pkg.name}: kind={kind} base={base_name} apks={len(splits)} "
        f"icon={icon_note} pkg={app.get('package')} vc={app.get('versionCode')}"
    )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aapt2", default=DEFAULT_AAPT2)
    parser.add_argument("--apkinfo", default=DEFAULT_APKINFO)
    parser.add_argument("packages", nargs="+")
    args = parser.parse_args()

    if not Path(args.aapt2).exists():
        print(f"aapt2 not found: {args.aapt2} (set AAPT2=...)", file=sys.stderr)
        return 2
    if not Path(args.apkinfo).exists():
        print(f"apkinfo not found: {args.apkinfo} (set APKINFO=...)", file=sys.stderr)
        return 2

    targets: list[Path] = []
    for pattern in args.packages:
        matched = [Path(p) for p in glob.glob(pattern)]
        targets.extend(matched if matched else [Path(pattern)])
    targets = [p for p in targets if p.exists()]
    if not targets:
        print("no package matched", file=sys.stderr)
        return 2

    problems: list[str] = []
    matched = 0
    for pkg in targets:
        found = check_one(args.aapt2, args.apkinfo, pkg)
        if found:
            problems.extend(found)
        else:
            matched += 1

    print()
    print(f"{matched}/{len(targets)} containers matched aapt2 + ZIP cross-check")
    for problem in problems:
        print(f"  FAIL {problem}")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
