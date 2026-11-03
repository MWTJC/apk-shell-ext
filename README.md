# apk-shell-ext

Windows 资源管理器壳扩展：让手机安装包在资源管理器里**显示应用自带的 launcher 图标（缩略图）**，并在**右键 → 属性**里提供一个信息标签页（形态参照 HashTab）。

| 项 | 值 |
| --- | --- |
| 阶段 | **M1 已完成（R-0002）；M2 进行中**（`apkinfo.exe --json` 可用；**TD-001 已关闭**；签名块未做） |
| 平台 | Windows 10 1809+ / Windows 11 x64（M5 才评估 x86 宿主） |
| 产物 | `apkthumb.dll`（缩略图，✅）· `apkinfo.exe`（解析 worker，🟡 骨架可用）· `apkprop.dll`（属性页，M3） |
| 许可 | 待定（建议 MIT；miniz 为 MIT，已 vendor 在 `src/third_party/miniz`，见 ADR-0004） |

## 一句话设计

> **图标只在要画图时读几十 KB；元数据只在打开属性页时全量解析；解析永远不发生在 explorer.exe 里。**

## 目标 / 非目标

**目标**

1. 大图标 / 超大图标 / 内容视图里，`.apk` 显示的是包内 launcher icon；小图标 / 列表 / 详细信息视图显示**按格式区分的静态图标**（APK / IPA / HAP / AAB / APPX 各一枚，`DefaultIcon`，零调用成本）。
2. 未打开过安装包的目录，浏览、滚动、排序、悬停**一次都不读原文件**（性能与未安装本扩展时相同）。
3. 右键属性多出「安装包信息」页：包名、应用名、版本、SDK、ABI、密度、语言、权限、签名摘要、哈希；**打开页面才开始解析**，页面先出、字段渐进填充。

**非目标（v1 明确不做）**

- 不做安装器 / adb 推送 / 应用商店查询 / 联网。
- 不做详情列、排序、悬浮提示、预览窗格（这些都要按目录批量调，性能代价与 P1 冲突，见 ADR-0002）。
- 不改写 APK / IPA / HAP 文件，不重签名。

## 目录

```
apk-shell-ext/
├─ docs/            设计、架构、格式矩阵、路线图、验收、ADR
├─ src/
│  ├─ core/         解析核心（ByteSource / zip 中央目录、AXML、ARSC、图标定位、WIC、缓存）— ✅ M1
│  ├─ thumbs/       IThumbnailProvider 壳扩展 — ✅ M1
│  ├─ propsheet/    IShellPropSheetExt 壳扩展 — M3
│  ├─ worker/       apkinfo.exe（JSON 输出）— M2
│  └─ third_party/  miniz（raw deflate，MIT，只编 C 源）
├─ tools/           build.cmd（vcvars+CMake+Ninja）、register.ps1（装/卸）
└─ tests/           gen_fixtures.py（aapt2 造真实包）、thumbtest.cpp（验收工具）
```

## 快速上手（M1 已可跑）

```bat
:: 1) 构建（需要 VS 2022/2026 的 MSVC，脚本里写死了 vcvarsall 路径，换机器改 tools\build.cmd）
tools\build.cmd Release                :: 产物在 out\x64\{apkthumb.dll,thumbtest.exe}

:: 2) 造测试包（需要 Android SDK 的 aapt2 + android.jar；路径可用 AAPT2 / ANDROID_JAR 覆盖）
python tests\gen_fixtures.py

:: 3) 单元验收（直接驱动 provider，不依赖 explorer）
out\x64\thumbtest.exe --probe tests\fixtures\generated\f_png.apk
out\x64\thumbtest.exe --file  tests\fixtures\generated\f_webp.apk --out C:\tmp\w.png
out\x64\thumbtest.exe --dir   tests\fixtures\generated --repeat 3 --json

:: 4) 注册（免 UAC，写 HKCU\Software\Classes）→ 资源管理器切"大图标"即可看到包内图标
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install
out\x64\thumbtest.exe --shell --file D:\somewhere\app.apk     :: 走 Explorer 同一条管线
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Uninstall
```

诊断日志：存在 `%TEMP%\apk-shell-ext.debug.on` 时写 `%LOCALAPPDATA%\apk-shell-ext\apkthumb.log`。
清缓存：`out\x64\thumbtest.exe --clean-cache`。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/design.md](docs/design.md) | **设计出发点**：调用频率、进程边界、冷热分离、可度量、既有方案实测 |
| [docs/architecture.md](docs/architecture.md) | 组件与进程边界、三个产物契约、JSON schema、注册表契约、缓存 |
| [docs/formats.md](docs/formats.md) | APK/APKS/XAPK/APKM/AAB/IPA/HAP/APPX 的容器与图标位置矩阵、安全限额 |
| [docs/roadmap.md](docs/roadmap.md) | **实现阶段 M0–M5**、每阶段交付/验收/退出条件、风险登记 |
| [docs/verification.md](docs/verification.md) | 把「无感」变成可测判据：ProcMon 配方、耗时与内存预算、记录表（R-0002 = M1 实测） |
| [docs/adr/](docs/adr/README.md) | 架构决策记录（5 条；前四条冻结，0005 由 M1 实测得出） |

## 当前状态

**M1 已完成（2026-11-03）**：`.apk` 缩略图闭环全部落地——CMake/Ninja 工程（`/MT`、`/W4 /WX`）、
`apkthumb.dll`（279 KB，只导入系统 DLL）、`tools\register.ps1`（HKCU 免 UAC，卸载零残留）、
自建正/负缓存、`tests\thumbtest.exe` 验收工具、`tests\gen_fixtures.py`（aapt2 造真实包）。

**实测数字**（[verification.md §10](docs/verification.md)）：100 个包冷 P50 8.2 ms / P95 14.9 ms，热 P50 1.3 ms；
走真实 Explorer 管线（`--shell`，经 dllhost）冷 12–21 ms、热 < 3 ms；8 个坏包全部优雅失败、进程不崩。

**M1 的关键意外**：缩略图 handler 并不在 explorer.exe 里跑，而是被放进 `dllhost.exe` 隔离宿主，
且只通过 `IInitializeWithStream` 喂数据（只实现 `IInitializeWithItem` 时系统返回 `0x8004B200`）。
这迫使解析核心改成"数据源无关"（文件 / 流），并加了一条带中央目录摘要的缓存键——见
[ADR-0005](docs/adr/0005-isolated-thumbnail-host-and-stream-input.md)。

**下一步 M2**：解析核心 + `apkinfo.exe --json`，把全量元数据搬出 DLL，并**关闭 TD-001**（ARSC 改分段流式）。

**下次开工清单（M2，按顺序）**

1. `src/core` 补齐：签名块（v2/v3 方案识别 + 证书摘要）、权限/组件计数、ABI/密度/locale 汇总、
   `--hash=sha256`（与 `certutil` 对照）、JSON v1 输出（字段只增不改）。
2. ARSC 分段流式读取，删除"整读 ≤ 8 MB"路径 → TD-001 关闭，且 ≥ 32 MB 资源表仍满足 V2。
3. `.apks/.xapk/.apkm`（外层 ZIP + `base.apk`）顺带做掉（M4a，复用 APK reader）。
4. 安全限额落地 + 3 个构造样本（条目炸弹 / 超大 arsc / 路径穿越）的拒绝用例。

## 变更纪律（写给自己）

1. 每条技术断言要能指到出处：实测（二进制/注册表/ProcMon）、官方文档或对方源码。文档里不写"据说"。
2. 任何影响调用频率或进程边界的改动，先写 ADR 再改代码。
3. 每阶段结束在 `docs/verification.md` 追加一条记录（含命令与结论），未验证项单列，不许含糊。
