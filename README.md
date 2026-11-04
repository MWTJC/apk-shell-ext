# apk-shell-ext

Windows 资源管理器壳扩展：让手机安装包在资源管理器里**显示应用自带的 launcher 图标（缩略图）**，并在**右键 → 属性**里提供一个信息标签页（形态参照 HashTab）。

| 项 | 值 |
| --- | --- |
| 阶段 | **M0–M5 全部完成**（M1 R-0002 · M2 R-0005 · M3 R-0006/0009/0010/0011 · M4 R-0011/R-0012/R-0013 · **M5 R-0014**；格式范围见 ADR-0006，分发见 ADR-0007） |
| 平台 | Windows 10 1809+ / Windows 11 x64（x86 宿主仍未评估） |
| 产物 | `apkthumb.dll`（缩略图）· `apkinfo.exe`（解析 worker + 状态/诊断/缓存/语言命令）· `apkprop.dll`（属性页）· **`apk-shell-ext-setup-1.0.0.exe`（单文件安装器，2.1 MB）** |
| 许可 | MIT（`LICENSE`；miniz 为 MIT，已 vendor 在 `src/third_party/miniz`，见 ADR-0004） |

## 一句话设计

> **图标只在要画图时读几十 KB；元数据只在打开属性页时全量解析；解析永远不发生在 explorer.exe 里。**

## 目标 / 非目标

**目标**

1. 大图标 / 超大图标 / 内容视图里，安装包显示的是自己的 launcher icon（APK / XAPK / HAP）；小图标 / 列表 / 详细信息视图显示**按格式区分的静态图标**（APK / XAPK / HAP 各一枚，`DefaultIcon`，零调用成本）。
2. 未打开过安装包的目录，浏览、滚动、排序、悬停**一次都不读原文件**（性能与未安装本扩展时相同）。
3. 右键属性多出「安装包信息」页：包名、应用名、版本、SDK、ABI、密度、语言、权限、签名摘要、哈希；**打开页面才开始解析**，页面先出、字段渐进填充。
4. 交付形态像个正经软件：**双击装（零 UAC）、应用列表里能卸、卸完零残留**；界面中英双语。

**非目标（v1 明确不做）**

- 不做安装器以外的东西：不做 adb 推送 / 应用商店查询 / 联网。
- 不做详情列、排序、悬浮提示、预览窗格（这些都要按目录批量调，性能代价与 P1 冲突，见 ADR-0002）。
- 不做 `IExtractIcon`（让小图标也显示包内图标）——见 [m5-delivery.md §6](docs/m5-delivery.md)。
- 不改写 APK / IPA / HAP 文件，不重签名。

## 目录

```
apk-shell-ext/
├─ docs/            设计、架构、格式矩阵、路线图、验收、ADR、安装说明
├─ installer/       Inno Setup 6 脚本（apk-shell-ext.iss）— M5
├─ icons/           三枚格式 ico（16/24/32/48/256，由 tools/gen_icons.py 生成）— M5
├─ src/
│  ├─ core/         解析核心（ByteSource / zip 中央目录、AXML、ARSC、签名块、图标定位、
│  │                容器侦查、WIC、缓存、i18n）— ✅ M1/M2/M4a/M5
│  ├─ thumbs/       IThumbnailProvider 壳扩展 — ✅ M1
│  ├─ propsheet/    IShellPropSheetExt 壳扩展（原生控件单页）— ✅ M3
│  ├─ worker/       apkinfo.exe（JSON 输出 + --status/--diag/--clear-cache/--lang）— ✅ M2/M5
│  ├─ version.h     版本号唯一来源（VERSIONINFO / 安装器都取它）— M5
│  └─ third_party/  miniz（raw deflate，MIT，只编 C 源）
├─ tools/           build.cmd、pack.cmd（出安装包）、register.ps1、verify-install.ps1、
│                   gen_icons.py、验收/截图脚本
└─ tests/           gen_fixtures.py（aapt2 造真实包）、verify_*.py（交叉校验）、验收工具
```

## 快速上手

```bat
:: 1) 构建（需要 VS 的 MSVC，脚本里写死了 vcvarsall 路径，换机器改 tools\build.cmd）
tools\build.cmd Release                :: 产物在 out\x64\{apkthumb.dll, apkprop.dll, apkinfo.exe, ...}

:: 2) 图标（一次性；已入库，除非改了生成脚本）
uv run --with pillow python tools\gen_icons.py

:: 3) 出安装包（需要 Inno Setup 6；ISCC 路径可用环境变量 ISCC 覆盖）
tools\pack.cmd                         :: 产物 out\dist\apk-shell-ext-setup-1.0.0.exe

:: 4) 装（零 UAC，只给当前用户）/ 卸
out\dist\apk-shell-ext-setup-1.0.0.exe
"%LOCALAPPDATA%\Programs\apk-shell-ext\unins000.exe"

:: 4b) 或者不装，就地注册当前 out\x64（开发调试用，写 HKCU）
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Uninstall
```

**造测试包 / 验收**（真实样本 `tests\fixtures\real\` 与 `generated\` 都不入库）：

```bat
python tests\gen_fixtures.py                              :: 4 正常 + 8 坏包（需 aapt2 + android.jar）
python tests\gen_signed_fixtures.py                        :: v1/v2/v3 三个签名夹具
python tests\gen_big_arsc.py                               :: 19.7 MB 真 arsc + deflate 变体
python tests\gen_container_fixtures.py --real <xapk>       :: 坏容器 / 坏 HAP

python tests\verify_apkinfo.py tests\fixtures\generated\*.apk                    :: 6/6
python tests\verify_apkinfo.py tests\fixtures\real\*.apk                         :: 3/3
python tests\verify_signature.py --glob "tests\fixtures\generated\signed\*.apk"   :: 3/3
python tests\verify_container.py "tests\fixtures\real\*.xapk"                    :: 2/2
python tests\verify_hap.py "tests\fixtures\real\*.hap"                           :: 2/2
out\x64\propsheet_model_test.exe --selftest                                      :: 71 ok / 0 fail
out\x64\thumbtest.exe --dir tests\fixtures\generated --repeat 1 --json           :: 1 个预期失败
out\x64\thumbtest.exe --shell --file <pkg>                                       :: 走 Explorer 同一条管线

:: 安装器验收（装前快照 -> 装 -> 断言 -> 卸 -> 断言 -> 快照 diff 必须为空）
powershell -File tools\verify-install.ps1 -Phase snapshot -Path a.txt
out\dist\apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
powershell -File tools\verify-install.ps1 -Phase check -State installed
"%LOCALAPPDATA%\Programs\apk-shell-ext\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
powershell -File tools\verify-install.ps1 -Phase check -State removed
powershell -File tools\verify-install.ps1 -Phase snapshot -Path b.txt
powershell -File tools\verify-install.ps1 -Phase diff -A a.txt -B b.txt
```

诊断与维护（装好之后，入口只有一个 exe）：

```bat
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --status
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag on|off|status|clear
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --clear-cache | --cache-info
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --lang zh|en|auto
```

日志：存在 `%TEMP%\apk-shell-ext.debug.on` 时写 `%LOCALAPPDATA%\apk-shell-ext\apkthumb.log`。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/design.md](docs/design.md) | **设计出发点**：调用频率、进程边界、冷热分离、可度量、既有方案实测 |
| [docs/architecture.md](docs/architecture.md) | 组件与进程边界、三个产物契约、JSON schema、注册表契约、缓存 |
| [docs/formats.md](docs/formats.md) | APK/XAPK/HAP 的容器与图标位置矩阵、安全限额（AAB/IPA/APPX 按 ADR-0006 不做） |
| [docs/roadmap.md](docs/roadmap.md) | **实现阶段 M0–M5** 与每阶段的实施记录、风险、技术债 |
| [docs/verification.md](docs/verification.md) | 验收判据、可复现配方、**R-0001 ~ R-0014 记录表**（含未验证项） |
| [docs/m3-property-page.md](docs/m3-property-page.md) | M3 属性页设计与实施记录（`IShellExtInit` 硬要求、图标退路、实测数字） |
| [docs/m5-delivery.md](docs/m5-delivery.md) | **M5 交付**：安装器 / 图标 / VERSIONINFO / i18n / 命令、实测数字、未做清单 |
| [docs/install.md](docs/install.md) | 用户手册（装/卸/静默/便携/FAQ）——**会随安装包落地到安装目录** |
| [docs/adr/](docs/adr/README.md) | 架构决策记录（7 条；0007 = 用 Inno Setup + 按用户安装） |

## 当前状态

**M1（2026-11-03）**：`.apk` 缩略图闭环——CMake/Ninja（`/MT`、`/W4 /WX`）、`apkthumb.dll`（只导入系统 DLL）、
`tools\register.ps1`（HKCU 免 UAC、卸载零残留）、自建正/负缓存、`tests\thumbtest.exe`、`tests\gen_fixtures.py`。
实测：100 个包冷 P50 8.2 ms / P95 14.9 ms，热 P50 1.3 ms；真实 Explorer 管线冷 12–21 ms。
**关键意外**：缩略图 handler 跑在 `dllhost.exe` 隔离宿主里，且只通过 `IInitializeWithStream` 喂数据
（只实现 `IInitializeWithItem` 时系统返回 `0x8004B200`）→ 解析核心改成数据源无关，见 [ADR-0005](docs/adr/0005-isolated-thumbnail-host-and-stream-input.md)。

**M2（2026-11-04）**：`apkinfo.exe --json`（schema v1）、ARSC 分段流式（TD-001 关闭：19.7 MB 真表 0.7–1.5 ms）、
签名块 v1/v2/v3（与 apksigner 22/22）、`features`、`--hash=sha256`（与 certutil 一致）、退出码 0/2/3、8 s watchdog；
与 `aapt2 dump badging` 11/11 一致。

**M3（2026-11-04）**：`apkprop.dll` 单页（**原生控件**：STATIC 头 + SysListView32 两列分组 + BUTTON 底栏；
权限/特性可折叠、默认收起；配色/深色跟随宿主）。`AddPages` 零 IO → 线程 → Job 里的 `apkinfo.exe` → `WM_APP` 回填；
关窗即 `TerminateJobObject`。7.9 MB 包 140–188 ms 出全页，1.79 GB 包 157 ms，按需 SHA-256 172 ms。
**计划外发现**：这个 build 在建页前先 `QI(IShellExtInit)`，缺了会被静默跳过 → 已补
（[m3-property-page.md §14](docs/m3-property-page.md)）。

**M4（2026-11-04）**：`.xapk/.apks/.apkm`（外层 ZIP + 内层 base.apk，Stored 的 base 走零拷贝窗口——
1.25 GB 的 PUBG 只读 107 MB）+ 鸿蒙 `.hap`（`module.json` + `$media:` 三级启发式 + `resources.index` 解应用名）。
`container` / `hap` 块进 JSON，属性页有「分包 · N 项」「安装包类型」等。其余格式按
[ADR-0006](docs/adr/0006-format-scope-cut.md) 不做。

**M5（2026-11-04，记录 [verification.md R-0014](docs/verification.md)｜设计 [m5-delivery.md](docs/m5-delivery.md)）**

- **安装器换成 Inno Setup**（[ADR-0007](docs/adr/0007-inno-setup-per-user-install.md)）：默认**按用户装、零 UAC**
  （`%LOCALAPPDATA%\Programs\apk-shell-ext` + `HKCU\Software\Classes`），勾"为所有用户安装"或 `/ALLUSERS`
  才要一次 UAC。实测 `/VERYSILENT` 装 → `verify-install.ps1` 全绿 → `unins000.exe` 卸 → 残留检查 ok、
  装前/卸后快照 **diff 为 0**。自研的 `apkinst.exe`（约 1100 行）已删除，教训写进 m5-delivery.md §1。
- **三枚格式图标**（`icons\{apk,apks,hap}.ico`，16/24/32/48/256 各档独立绘制）+ `DefaultIcon` 注册：
  列表视图 16 px 下绿机器人 / 靛紫双卡 / 红六边形三族可分辨（`docs/evidence/m5-defaulticons-list-zoom.png`）。
- **VERSIONINFO 1.0.0.0**（四个二进制，en-US + zh-CN 双块）；`APKSX_PARSER_VERSION` 升到 `m5`，旧缩略图缓存自动失效。
- **装好之后的命令都并入 `apkinfo.exe`**：`--status / --diag / --clear-cache / --cache-info / --lang`。
- **中英双语**：`src/core/i18n.{h,cpp}`（界面 77 条 + 解析器错误字典约 60 条），语言顺序
  `UiLanguage`(HKCU) → `APKSX_LANG` → 系统语言；属性页中英各截一张
  （`m5-propsheet-zh.png` / `m5-propsheet-en.png`），model 单测 60 → 71 ok。
- **`IExtractIcon` 结论：不做**（ADR-0001 不修订）。

## 下一步（v1 之后的待办，按价值排序）

1. **代码签名**（OV/EV 待定，见 design.md §8）：v1 无证书，SmartScreen 会提示"未知发布者"。
2. **`/ALLUSERS` 整机安装实跑一次**：`.iss` 的提权分支只做了静态审阅（自动化里弹 UAC 会卡会话）。
3. **修 `proptest.exe`**：本机（Windows 11 26300）`PropertySheetW` 立即返回 −1，换 M4 旧 DLL 同样失败；
   在修好之前，属性页验收用真实资源管理器对话框截图。
4. **ProcMon 复跑 V1/V9**：M5 没改缩略图代码路径、`DefaultIcon` 纯静态，但仍应测一次"0 次 CreateFile"。
5. HAP 增强（非阻塞）：layered icon 合成、`$media:<id>` 的 id→文件名映射、`.hsp`/`.app` 套件、HAP 签名块。
6. TD-002（deflate 的大 arsc）；V8 恶意样本回归；长跑（句柄 / 负缓存过期）。
7. 用户已明确挂起（2026-11-04）：真实 150%/200% DPI 人眼确认。

## 接力说明（下一会话从这里开始，2026-11-04 收口）

当前状态一句话：**M0–M5 全部收口**，`apk-shell-ext-setup-1.0.0.exe` 可装可用，装/卸零残留已验证；
剩下的都是"锦上添花 + 补测"，没有阻塞项。

- **构建**：`tools\build.cmd Release` → `out\x64\{apkthumb.dll, apkprop.dll, apkinfo.exe, thumbtest.exe,
  proptest.exe, propsheet_model_test.exe}`。坑：`apkprop.dll` 被 explorer 占用时链接会 `LNK1104`——
  先 `Stop-Process -Name explorer -Force` 再编，编完 explorer 会自启。
- **打包**：`tools\pack.cmd`（内部 ISCC，本机 `E:\PROGRAMS\Inno Setup 6`；换机器设 `%ISCC%`）→ `out\dist\`。
- **夹具**：`tests\gen_*.py` 可随时重造（不入库）；真实样本在 `tests\fixtures\real\`（gitignore），
  换机器要重新准备：3 个普通 APK（aida64 / qwxds / wwlocation-probe）、2 个 `.xapk`
  （Poweramp 22 MB；PUBG 1.25 GB）、2 个 `.hap`（ShuaTiBao108、entry-default-unsigned）、`big\`（1.26 G / 1.92 G）。
- **验收全套**：见上面「快速上手」里的命令块，**加上** `verify-install.ps1` 的装/卸/快照三步。
- **本机当前状态**：已用安装器按用户装好（`%LOCALAPPDATA%\Programs\apk-shell-ext`），
  `UiLanguage` 恢复 `auto`（= 中文），图标缓存清过。要回到"干净机器"就 `unins000.exe`。
- **改图标/换 DLL 后**：删 `%LOCALAPPDATA%\IconCache.db` 与
  `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db` 并重启资源管理器，否则看的是旧图（踩过）。
- **改文案**：界面文案只改 `src/core/i18n.cpp` 的表；解析器错误保留 core 里的中文原文，
  由 `TrMsg` 字典在 worker 出口翻译。`propsheet_model_test --selftest` 会钉住中文断言。

## 变更纪律（写给自己）

1. 每条技术断言要能指到出处：实测（二进制/注册表/ProcMon）、官方文档或对方源码。文档里不写"据说"。
2. 任何影响调用频率或进程边界的改动，先写 ADR 再改代码。
3. 每阶段结束在 `docs/verification.md` 追加一条记录（含命令与结论），未验证项单列，不许含糊。
4. 安装/卸载行为属于交付契约：改了必须同步 `docs/install.md`（它会随安装包发给用户）与 `verify-install.ps1`。
