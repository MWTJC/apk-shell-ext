#!/usr/bin/env python3
"""把 apkinfo.exe 的 signature 字段与 apksigner verify 对照。

判据（只比"可显示的信息"，不比摘要校验结果）：
  1. 方案集合：我们识别出的 v1/v2/v3/v3.1 必须与 apksigner "Verified using ...: true" 集合相等；
  2. 证书 SHA-256：集合相等（v4 是独立 .idsig 文件，不在 APK 内，apksigner 报 false 也无所谓）。

用法：
  python tests/verify_signature.py <apk...>
  python tests/verify_signature.py --glob "tests/fixtures/real/*.apk"

退出码：0 全部一致 / 1 有差异 / 2 用法错误
"""
import argparse
import glob
import zipfile
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APKINFO = ROOT / "out" / "x64" / "apkinfo.exe"
SDK = Path(os.environ.get("ANDROID_SDK", r"E:\PROGRAMS\Android\Sdk"))
APKSIGNER = Path(os.environ.get("APKSIGNER", SDK / "build-tools" / "36.0.0" / "apksigner.bat"))

SCHEME_RE = re.compile(r"Verified using (v3\.1|v1|v2|v3) scheme[^:]*:\s*(true|false)")
CERT_RE = re.compile(r"Signer #\d+ certificate SHA-256 digest:\s*([0-9a-f]{64})")


def ours(path):
    proc = subprocess.run([str(APKINFO), "--json", str(path)], capture_output=True)
    if proc.returncode != 0:
        return None, proc.stderr.decode("utf-8", "replace").strip()
    info = json.loads(proc.stdout.decode("utf-8"))
    sig = info.get("signature") or {}
    schemes = set(sig.get("scheme") or [])
    certs = {c["sha256"].lower() for c in sig.get("certs") or []}
    return (schemes, certs), None


def v1_superseded(path):
    """`.SF` 里写了 X-Android-APK-Signed: 2/3 表示"本包另有 v2/v3 签名"，
    验证器因此跳过 v1（apksigner 报 v1=false），但 v1 块本身仍然存在。
    我们只做结构识别，所以这种情况算"解释得通"，不算差异。"""
    try:
        with zipfile.ZipFile(path) as z:
            for name in z.namelist():
                up = name.upper()
                if up.startswith("META-INF/") and up.endswith(".SF"):
                    if b"X-Android-APK-Signed" in z.read(name):
                        return True
    except Exception:
        return False
    return False


def reference(path):
    proc = subprocess.run([str(APKSIGNER), "verify", "--verbose", "--print-certs", str(path)],
                          capture_output=True)
    out = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    if "DOES NOT VERIFY" in out or proc.returncode != 0:
        # 未签名 / 签名损坏：预期我们也不报任何方案
        return (set(), set()), out.splitlines()[0] if out.splitlines() else "verify failed"
    schemes = {m.group(1) for m in SCHEME_RE.finditer(out) if m.group(2) == "true"}
    certs = {m.group(1).lower() for m in CERT_RE.finditer(out)}
    return (schemes, certs), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("apks", nargs="*")
    ap.add_argument("--glob", action="append", default=[])
    args = ap.parse_args()

    files = [Path(a) for a in args.apks]
    for pattern in args.glob:
        files += [Path(p) for p in glob.glob(pattern)]
    files = [f for f in files if f.exists()]
    if not files:
        ap.error("no apk given")

    bad = 0
    for path in files:
        got, err = ours(path)
        if got is None:
            # 坏包：两边都拒绝才算一致（我们拒收 = 没有签名信息可显示）
            want, _ = reference(path)
            if not want[0] and not want[1]:
                print(f"[OK  ] {path.name}  两边都拒绝（{err}）")
            else:
                print(f"[FAIL] {path.name}: apkinfo 失败但 apksigner 认得: {err}")
                bad += 1
            continue
        want, note = reference(path)
        ok = got[0] == want[0] and got[1] == want[1]
        extra_schemes = got[0] - want[0]
        explained = (not ok and extra_schemes == {"v1"} and got[0] >= want[0]
                     and got[1] == want[1] and v1_superseded(path))
        if explained:
            note = "v1 块存在但 .SF 声明了 v2/v3（验证器跳过 v1），按存在性一致处理"
            ok = True
        mark = "OK  " if ok else "DIFF"
        print(f"[{mark}] {path.name}")
        print(f"       ours  schemes={sorted(got[0])} certs={sorted(x[:8] for x in got[1]) or '[]'}")
        print(f"       apksigner schemes={sorted(want[0])} certs={sorted(x[:8] for x in want[1]) or '[]'}"
              + (f"  ({note})" if note else ""))
        if not ok:
            bad += 1
            if got[0] != want[0]:
                print(f"       scheme diff: missing={sorted(want[0] - got[0])} extra={sorted(got[0] - want[0])}")
            if got[1] != want[1]:
                print(f"       cert diff: missing={sorted(want[1] - got[1])} extra={sorted(got[1] - want[1])}")

    print(f"\n{len(files) - bad}/{len(files)} matched")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
