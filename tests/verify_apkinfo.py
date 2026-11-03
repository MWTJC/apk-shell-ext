#!/usr/bin/env python3
"""Cross-check apkinfo.exe against aapt2 (M2 acceptance, docs/roadmap.md M2).

Compares package / versionCode / versionName / minSdk / targetSdk / native-code /
uses-permission / application-label for every APK given on the command line.

Usage:
    python tests/verify_apkinfo.py [--aapt2 PATH] [--apkinfo PATH] <apk> [<apk> ...]

Exit code 0 = all packages matched, 1 = differences found, 2 = harness error.
Differences are printed in a table so they can be triaged (ours may legitimately
show MORE than aapt2, e.g. permissions merged from sdk-23 variants).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_AAPT2 = os.environ.get(
    "AAPT2", "E:/PROGRAMS/Android/Sdk/build-tools/36.0.0/aapt2.exe"
)
DEFAULT_APKINFO = os.environ.get("APKINFO", str(ROOT / "out" / "x64" / "apkinfo.exe"))


def run(aapt2: str, apk: Path) -> dict:
    out = subprocess.run(
        [aapt2, "dump", "badging", str(apk)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if out.returncode != 0:
        raise RuntimeError(f"aapt2 failed on {apk}: {out.stderr[:400]}")
    text = out.stdout
    data: dict = {"permissions": set(), "abis": set()}

    m = re.search(r"package: name='([^']*)' versionCode='([^']*)' versionName='([^']*)'", text)
    if m:
        data["package"] = m.group(1)
        data["versionCode"] = int(m.group(2) or 0)
        data["versionName"] = m.group(3)

    for key in ("sdkVersion", "minSdkVersion"):
        m = re.search(rf"{key}:'(\d+)'", text)
        if m:
            data["minSdk"] = int(m.group(1))
            break
    m = re.search(r"targetSdkVersion:'(\d+)'", text)
    if m:
        data["targetSdk"] = int(m.group(1))
    m = re.search(r"application-label:'((?:[^'\\]|\\.)*)'", text)
    if m:
        data["label"] = m.group(1)
    m = re.search(r"native-code: (.+)", text)
    if m:
        data["abis"] = set(re.findall(r"'([^']+)'", m.group(1)))
    for name in re.findall(r"uses-permission: name='([^']+)'", text):
        data["permissions"].add(name)
    return data


def ours(apkinfo: str, apk: Path) -> dict:
    out = subprocess.run(
        [apkinfo, "--json", str(apk)],
        capture_output=True,
        encoding="utf-8",
        errors="replace",
    )
    if out.returncode != 0:
        raise RuntimeError(f"apkinfo failed on {apk} (rc={out.returncode}): {out.stderr[:400]}")
    doc = json.loads(out.stdout)
    app = doc.get("app", {})
    return {
        "package": app.get("package"),
        "versionCode": app.get("versionCode", 0),
        "versionName": app.get("versionName"),
        "minSdk": app.get("minSdk", 0),
        "targetSdk": app.get("targetSdk", 0),
        "label": app.get("label"),
        "abis": set(app.get("abis", [])),
        "permissions": {p["name"] for p in doc.get("permissions", [])},
        "icon": doc.get("icon", {}).get("source"),
        "elapsedMs": doc.get("diagnostics", {}).get("elapsedMs"),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("apks", nargs="+")
    ap.add_argument("--aapt2", default=DEFAULT_AAPT2)
    ap.add_argument("--apkinfo", default=DEFAULT_APKINFO)
    args = ap.parse_args()

    if not Path(args.aapt2).exists():
        print(f"aapt2 not found: {args.aapt2}", file=sys.stderr)
        return 2
    if not Path(args.apkinfo).exists():
        print(f"apkinfo not found: {args.apkinfo}", file=sys.stderr)
        return 2

    failures = 0
    for raw in args.apks:
        apk = Path(raw)
        try:
            ref = run(args.aapt2, apk)
            got = ours(args.apkinfo, apk)
        except Exception as exc:  # noqa: BLE001 - harness reports everything
            print(f"{apk.name}: ERROR {exc}", file=sys.stderr)
            failures += 1
            continue

        diffs = []
        for key in ("package", "versionCode", "versionName", "minSdk", "targetSdk", "label"):
            if ref.get(key) != got.get(key):
                diffs.append(f"{key}: aapt2={ref.get(key)!r} ours={got.get(key)!r}")
        if ref["abis"] != got["abis"]:
            diffs.append(f"abis: aapt2={sorted(ref['abis'])} ours={sorted(got['abis'])}")
        if ref["permissions"] != got["permissions"]:
            only_ref = sorted(ref["permissions"] - got["permissions"])
            only_ours = sorted(got["permissions"] - ref["permissions"])
            diffs.append(f"permissions: only-aapt2={only_ref} only-ours={only_ours}")

        status = "OK  " if not diffs else "DIFF"
        print(
            f"{status} {apk.name:<28} {got.get('package'):<28} "
            f"{got.get('versionName')} ({got.get('versionCode')}) "
            f"sdk {got.get('minSdk')}/{got.get('targetSdk')} "
            f"icon={got.get('icon')} {got.get('elapsedMs')}ms"
        )
        for d in diffs:
            print(f"       {d}")
        if diffs:
            failures += 1

    print(f"\n{len(args.apks) - failures}/{len(args.apks)} packages matched")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
