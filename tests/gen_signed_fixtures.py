#!/usr/bin/env python3
"""造"签名方案覆盖"夹具：v1-only / v2-only / v3-only 的真实签名包。

为什么要造：手边的真实包只覆盖 v2、v2+v3（见 MANIFEST.json），没有 v1-only 与 v3-only。
签名块解析的三种形状必须各有一个包对得上 apksigner，否则"实现对了但没测过"。

输入：tests/fixtures/generated/f_png.apk（gen_fixtures.py 造，未签名）
输出：tests/fixtures/generated/signed/s_v1.apk / s_v2.apk / s_v3.apk

依赖：Android SDK build-tools 里的 apksigner + keytool（均可用 JDK 21 跑）。
路径可用 APKSIGNER / KEYTOOL 环境变量覆盖。
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "tests" / "fixtures" / "generated"
OUT = GEN / "signed"
KS = OUT / "fixture.jks"
SRC = GEN / "f_png.apk"
ALIGNED = OUT / "_aligned.apk"  # 先对齐再签名（反过来的话，对齐会移动条目、破坏签名块）

SDK = Path(os.environ.get("ANDROID_SDK", r"E:\PROGRAMS\Android\Sdk"))
APKSIGNER = Path(os.environ.get("APKSIGNER", SDK / "build-tools" / "36.0.0" / "apksigner.bat"))
ZIPALIGN = Path(os.environ.get("ZIPALIGN", SDK / "build-tools" / "36.0.0" / "zipalign.exe"))
AAPT2 = Path(os.environ.get("AAPT2", SDK / "build-tools" / "36.0.0" / "aapt2.exe"))
ANDROID_JAR = Path(os.environ.get("ANDROID_JAR", SDK / "platforms" / "android-36" / "android.jar"))
LEGACY_MANIFEST = ROOT / "tests" / "fixtures" / "src" / "f_png" / "AndroidManifest.xml"
LEGACY_RES = ROOT / "tests" / "fixtures" / "src" / "f_png" / "res"
JAVA_HOME = Path(os.environ.get("JAVA_HOME", r"E:\PROGRAMS\Java\jdk-21.0.12.1"))
KEYTOOL = Path(os.environ.get("KEYTOOL", JAVA_HOME / "bin" / "keytool.exe"))

COMBOS = [
    ("s_v1.apk", True, False, False),
    ("s_v2.apk", False, True, False),
    ("s_v3.apk", False, False, True),
]


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def ensure_keystore():
    if KS.exists():
        return
    OUT.mkdir(parents=True, exist_ok=True)
    # 一次性的测试密钥：只用于夹具，密码写死没关系（不涉及任何真实资产）
    run([KEYTOOL, "-genkeypair", "-keystore", str(KS), "-alias", "fixture",
         "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
         "-storepass", "android", "-keypass", "android",
         "-dname", "CN=apk-shell-ext fixture, O=apk-shell-ext, C=CN"])


def build_legacy_input() -> Path:
    """targetSdk 28 的等价包：v1-only 的包在 targetSdk>=30 上会被 apksigner 本身拒收
    （"Target SDK version N requires a minimum of signature scheme v2"），
    所以要给 v1 夹具一个"老包"作为输入，否则没法拿 apksigner 做对照。"""
    compiled = OUT / "_res.zip"
    legacy = OUT / "_legacy.apk"
    run([AAPT2, "compile", "--dir", LEGACY_RES, "-o", compiled])
    run([AAPT2, "link", "-o", legacy, "-I", ANDROID_JAR, "--manifest", LEGACY_MANIFEST,
         "--min-sdk-version", "21", "--target-sdk-version", "28",
         "--rename-manifest-package", "com.example.legacyv1", compiled])
    compiled.unlink()
    return legacy


def main():
    if not SRC.exists():
        sys.exit(f"missing {SRC}; run tests/gen_fixtures.py first")
    OUT.mkdir(parents=True, exist_ok=True)
    ensure_keystore()
    run([str(ZIPALIGN), "-f", "4", str(SRC), str(ALIGNED)])
    # v1-only 用 targetSdk 28 的老包（见 build_legacy_input 的说明），对齐后同样落 ALIGNED
    legacy = build_legacy_input()
    v1_input = OUT / "_legacy_aligned.apk"
    run([str(ZIPALIGN), "-f", "4", str(legacy), str(v1_input)])

    for name, v1, v2, v3 in COMBOS:
        dst = OUT / name
        if dst.exists():
            dst.unlink()
        run([str(APKSIGNER), "sign",
             "--ks", str(KS), "--ks-pass", "pass:android", "--key-pass", "pass:android",
             "--min-sdk-version", "21", "--max-sdk-version", "35",
             f"--v1-signing-enabled={'true' if v1 else 'false'}",
             f"--v2-signing-enabled={'true' if v2 else 'false'}",
             f"--v3-signing-enabled={'true' if v3 else 'false'}",
             "--out", str(dst), str(v1_input if v1 else ALIGNED)])
        print(f"  -> {dst.relative_to(ROOT)} ({dst.stat().st_size:,} B)")

    ALIGNED.unlink()
    v1_input.unlink()
    legacy.unlink()
    print("done")


if __name__ == "__main__":
    main()
