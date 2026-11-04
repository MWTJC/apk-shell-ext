# apk-shell-ext

Windows 资源管理器壳扩展：让手机安装包在资源管理器里**显示应用自带的 launcher 图标（缩略图）**，并在**右键 → 属性**里提供一个信息标签页（形态参照 HashTab）。

| 项 | 值 |
| --- | --- |
| 阶段 | **M1 ✅（R-0002）· M2 ✅（R-0005，`splits` 随 M4a）· M3 ✅（R-0006/0009/0010/0011）· M4a ✅（.xapk/.apks/.apkm，R-0011）· M4d ✅（.hap，R-0012）** |
| 平台 | Windows 10 1809+ / Windows 11 x64（M5 才评估 x86 宿主） |
| 产物 | `apkthumb.dll`（缩略图，✅）· `apkinfo.exe`（解析 worker，✅ JSON v1）· `apkprop.dll`（属性页，✅） |
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
│  ├─ core/         解析核心（ByteSource / zip 中央目录、AXML、ARSC、签名块、图标定位、容器侦查、WIC、缓存）— ✅ M1/M2/M4a
│  ├─ thumbs/       IThumbnailProvider 壳扩展 — ✅ M1
│  ├─ propsheet/    IShellPropSheetExt 壳扩展（原生控件单页）— ✅ M3
│  ├─ worker/       apkinfo.exe（JSON 输出）— M2
│  └─ third_party/  miniz（raw deflate，MIT，只编 C 源）
├─ tools/           build.cmd（vcvars+CMake+Ninja）、register.ps1（装/卸）
└─ tests/           gen_fixtures.py（aapt2 造真实包）、thumbtest.cpp（验收工具）
```

## 快速上手（M1 已可跑）

```bat
:: 1) 构建（需要 VS 2022/2026 的 MSVC，脚本里写死了 vcvarsall 路径，换机器改 tools\build.cmd）
tools\build.cmd Release                :: 产物在 out\x64\{apkthumb.dll, apkinfo.exe, thumbtest.exe}

:: 2) 造测试包（需要 Android SDK 的 aapt2 + android.jar；路径可用 AAPT2 / ANDROID_JAR 覆盖）
python tests\gen_fixtures.py

:: 3) 签名夹具 + 与 apksigner / aapt2 的交叉校验（需要 SDK 的 apksigner、aapt2、keytool）
python tests\gen_signed_fixtures.py
python tests\verify_signature.py --glob "tests\fixtures\generated\signed\*.apk"
python tests\verify_apkinfo.py tests\fixtures\generated\*.apk

:: 3b) 多 APK 容器（.xapk/.apks/.apkm，M4a）：坏容器夹具 + 与 aapt2 交叉校验
python tests\gen_container_fixtures.py --real tests\fixtures\real\*.xapk
python tests\verify_container.py "tests\fixtures\real\*.xapk"

:: 3c) 鸿蒙 HAP（M4d）：没有官方 CLI，直接对包里的 module.json 交叉校验
python tests\verify_hap.py tests\fixtures\real\*.hap

:: 4) 单元验收（直接驱动 provider，不依赖 explorer）
out\x64\thumbtest.exe --probe tests\fixtures\generated\f_png.apk
out\x64\thumbtest.exe --file  tests\fixtures\generated\f_webp.apk --out C:\tmp\w.png
out\x64\thumbtest.exe --dir   tests\fixtures\generated --repeat 3 --json

:: 5) 注册（免 UAC，写 HKCU\Software\Classes；默认 .apk/.xapk/.apks/.apkm/.hap）→ 资源管理器切"大图标"即可看到包内图标
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
| [docs/m3-property-page.md](docs/m3-property-page.md) | **M3 属性页**：设计方案（已评审）+ 实施记录（`IShellExtInit` 硬要求、图标退路、实测数字） |
| [docs/adr/](docs/adr/README.md) | 架构决策记录（5 条；前四条冻结，0005 由 M1 实测得出） |

## 当前状态

**M1 已完成（2026-11-03）**：`.apk` 缩略图闭环全部落地——CMake/Ninja 工程（`/MT`、`/W4 /WX`）、
`apkthumb.dll`（285 KB，只导入系统 DLL；M2 加流式 arsc 后 301 KB）、`tools\register.ps1`（HKCU 免 UAC，卸载零残留）、
自建正/负缓存、`tests\thumbtest.exe` 验收工具、`tests\gen_fixtures.py`（aapt2 造真实包）。

**实测数字**（[verification.md §10](docs/verification.md)）：100 个包冷 P50 8.2 ms / P95 14.9 ms，热 P50 1.3 ms；
走真实 Explorer 管线（`--shell`，经 dllhost）冷 12–21 ms、热 < 3 ms；8 个坏包全部优雅失败、进程不崩。

**M1 的关键意外**：缩略图 handler 并不在 explorer.exe 里跑，而是被放进 `dllhost.exe` 隔离宿主，
且只通过 `IInitializeWithStream` 喂数据（只实现 `IInitializeWithItem` 时系统返回 `0x8004B200`）。
这迫使解析核心改成"数据源无关"（文件 / 流），并加了一条带中央目录摘要的缓存键——见
[ADR-0005](docs/adr/0005-isolated-thumbnail-host-and-stream-input.md)。

**M3 已完成（2026-11-04，记录 [verification.md R-0006](docs/verification.md)）**

`apkprop.dll`：右键属性里的「安装包信息」单页，**原生控件**（STATIC 头 + SysListView32 两列分组 + BUTTON 底栏，
样式/深色/选中/无障碍都交给系统；`权限`、`特性` 两组可折叠、默认收起）；
底色/文字/字体全部向属性窗口本身与系统色要，所以页内永远和窗口同风格（系统深色时也是浅色）。
重绘走双缓冲 + 脏矩形（hover 只重画 1–2 行），快速划过不闪；tooltip 悬停停留后才出，且只在该行真被截断时出。
`AddPages` 零 IO，`PSM_INITDIALOG` 之后起线程 → `apkinfo.exe`（挂在带 `KILL_ON_JOB_CLOSE` 的 Job 上）→
JSON 在后台线程变成行模型 → `WM_APP` 回填；关窗即 `TerminateJobObject`。
实测：7.9 MB 包 **140–188 ms** 出全页，1.79 GB 包 **157 ms**，按需 SHA-256 172 ms（1.9 GB 约 1.2 s），
关闭时正在算哈希 ×3 次无残留进程。

**计划外发现**：这个 Windows build 在建页之前先 `QI(IShellExtInit)`，只实现 `IShellPropSheetExt + IPersistFile`
的 handler 会被**静默跳过**（`AddPages` 根本不会被调用）。已补 `IShellExtInit::Initialize`，见
[docs/m3-property-page.md §14](docs/m3-property-page.md)。

**M2 已完成的六件（2026-11-04）**

1. `apkinfo.exe --json`（schema v1 的 `file`/`app`/`icon`/`components`/`permissions`/`diagnostics`），
   label 走 arsc 解析，`--hash=sha256` 与 `certutil` 一致，退出码 0/2/3 与 8 s watchdog 均实测。
2. **TD-001 关闭**：`resources.arsc` 改为在 Stored 条目上开窗口流式跳读；19.7 MB 真表 0.7–1.5 ms。
3. 大包测速：1.26 GB / 1.92 GB 的真包解析 3.8–6.9 ms，缩略图 37–41 ms（经 dllhost 172 ms）。
4. 与 `aapt2 dump badging` 逐字段交叉校验 **11/11 一致**（含混淆包、自适应图标包、GB 级包）。
5. **签名块**：v1（`META-INF/*.RSA|DSA|EC` 的 PKCS#7）+ v2/v3/v3.1（APK Signing Block）方案识别与证书 SHA-256；
   自造 v1-only / v2-only / v3-only 三个夹具，与 `apksigner verify --print-certs` **22/22 一致**。
   注意 `scheme` 的语义是"检测到的签名块"而非"校验通过"（见 [architecture.md §2.3](docs/architecture.md)）。
6. `features`（`uses-feature` + `android:required`）→ 与 aapt2 **11/11 一致**。

**M4a 已完成（2026-11-04，记录 [verification.md R-0011](docs/verification.md)）**

`.xapk/.apks/.apkm`（外层 ZIP + 内层 base.apk）走同一条解析路径：`src/core/package.cpp` 先判容器，
Stored 的 base 用 `SliceSource` 在外层文件上开窗口（**1.25 GB 的 PUBG xapk 只读 107 MB 的 base**），
deflate 的 base 整块解压且上限 512 MB。JSON 新增 `container` 块（`base` + `splits`）；
属性页多出「安装包类型」「内层 APK」两行与可折叠「分包 · N 项」组；`register.ps1` 默认注册四个扩展名。
实测：Poweramp xapk 缩略图 24 ms（经 shell 110 ms）、属性页 188–282 ms；
PUBG（1.25 GB）缩略图 35/20 ms（冷/热）、worker 9.5–12.4 ms；2/2 与 aapt2 一致；4 个坏容器全部优雅失败。
**注意**：`.apks`（bundletool）与 `.apkm`（APKMirror）只有启发式 + 坏样本，没有真实样本验证。

**M4d 已完成（2026-11-04，记录 [verification.md R-0012](docs/verification.md)）**

鸿蒙 `.hap`：`module.json`（JSON）直读（`src/core/hap.cpp`），判定放在 APK/容器之前；
图标走 `$media:` 三级启发式（名字 → layered json 的 `foreground`/`background` 键名 → `startWindowIcon`/约定名/最大 PNG）。
JSON 新增 `hap` 块（模块 / 主 Ability / min-targetAPI / 编译 SDK / 设备类型 / Ability 计数 / labelRef），
**不把鸿蒙 API 号塞进 `app.minSdk/targetSdk`**；属性页有模块/API/设备类型/Ability/权限，签名行如实写"未解析"。
实测：两个 HAP 与 `module.json` 交叉校验 2/2、缩略图 41 ms（经 shell 87/61 ms）、属性页 156–172 ms、worker < 2 ms。
**不做**：`$string:` 标签（要解 RestoolV2 的 `resources.index`）、layered 合成、`.hsp`/`.app` 套件、HAP 签名块。

**下一步（建议顺序）**

1. M4e：`.appx/.msix`（XML 清单 + 限定符选图，估 0.5 d，最便宜的一块）。
2. M4b：`.aab`（protobuf manifest，最贵的一块）→ M4c `.ipa`（binary plist / Assets.car）。
3. HAP 增强（可选）：`resources.index` 解析 → 应用名与 id→文件名映射；layered icon 合成。
4. M3 / M2 尾巴（用户 2026-11-04 明确说"算边缘情况，等遇到问题再说"）：真实 150%/200% 人眼确认、
   TD-002（deflate 的大 arsc）、V8 恶意样本回归、长跑。

**接力说明**（下一会话从这里开始）

- 构建：`tools\build.cmd Release` → `out\x64\{apkthumb.dll, apkinfo.exe, apkprop.dll, thumbtest.exe, proptest.exe}`。
- 夹具：`python tests\gen_fixtures.py`（4 正常 + 8 坏包）、`python tests\gen_big_arsc.py`（19.7 MB 真 arsc + deflate 变体）、
  `python tests\gen_container_fixtures.py --real <xapk>`（4 个坏容器）。
- 真实样本：`tests\fixtures\real\`（3 个普通包 + 2 个 `.xapk` + 2 个 `.hap` + `big\` 里 1.26 G / 1.92 G 各一个），**gitignore，不入库**。
- 验收：`python tests\verify_apkinfo.py <apk...>`（对 aapt2）、`python tests\verify_container.py "<xapk-glob>"`、
  `python tests\verify_hap.py "<hap-glob>"`、
  `out\x64\propsheet_model_test.exe --selftest`（行模型 + 布局/DPI 断言）、`out\x64\thumbtest.exe --dir <目录> --json`、
  `out\x64\thumbtest.exe --shell --file <apk>`（走 Explorer 管线）、`out\x64\apkinfo.exe --json <apk>`。
- 扩展当前**已注册**（HKCU）：`powershell -File tools\register.ps1 -Uninstall` 可撤；诊断日志需手动开 `%TEMP%\apk-shell-ext.debug.on`。

## 变更纪律（写给自己）

1. 每条技术断言要能指到出处：实测（二进制/注册表/ProcMon）、官方文档或对方源码。文档里不写"据说"。
2. 任何影响调用频率或进程边界的改动，先写 ADR 再改代码。
3. 每阶段结束在 `docs/verification.md` 追加一条记录（含命令与结论），未验证项单列，不许含糊。
