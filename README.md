# apk-shell-ext

Windows 资源管理器壳扩展：让手机安装包在资源管理器里**显示应用自带的 launcher 图标（缩略图）**，并在**右键 → 属性**里提供一个信息标签页（形态参照 HashTab）。

| 项 | 值 |
| --- | --- |
| 阶段 | **M0 已完成（2026-11-03 评审通过）；M1 待开工**（尚无代码） |
| 平台 | Windows 10 1809+ / Windows 11 x64（M5 才评估 x86 宿主） |
| 产物 | `apkthumb.dll`（缩略图）· `apkprop.dll`（属性页）· `apkinfo.exe`（解析 worker） |
| 许可 | 待定（建议 MIT；若引入第三方解析库，逐个核许可后再定，见 ADR-0004） |

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
│  ├─ core/         解析核心（zip 容器、AXML、ARSC、proto bundle、plist、鸿蒙 index）— M2
│  ├─ thumbs/       IThumbnailProvider 壳扩展 — M1
│  ├─ propsheet/    IShellPropSheetExt 壳扩展 — M3
│  └─ worker/       apkinfo.exe（JSON 输出）— M2
├─ tools/           注册/卸载脚本、缓存清理、ProcMon 配方、统计脚本
└─ tests/           固件包（自造）与断言
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/design.md](docs/design.md) | **设计出发点**：调用频率、进程边界、冷热分离、可度量、既有方案实测 |
| [docs/architecture.md](docs/architecture.md) | 组件与进程边界、三个产物契约、JSON schema、注册表契约、缓存 |
| [docs/formats.md](docs/formats.md) | APK/APKS/XAPK/APKM/AAB/IPA/HAP/APPX 的容器与图标位置矩阵、安全限额 |
| [docs/roadmap.md](docs/roadmap.md) | **实现阶段 M0–M5**、每阶段交付/验收/退出条件、风险登记 |
| [docs/verification.md](docs/verification.md) | 把「无感」变成可测判据：ProcMon 配方、耗时与内存预算、记录表 |
| [docs/adr/](docs/adr/README.md) | 架构决策记录（4 条，冻结前三条不可被后续阶段悄悄推翻） |

## 当前状态

**M0 已完成（2026-11-03 评审通过）**：文档与接口/验收基线冻结，三项决策已确认——① 小图标放弃包内图标、改用格式差异化静态 ico；② C++ / 静态 CRT / 无托管运行时（性能优先）；③ M1 允许"整读 `resources.arsc`（≤ 8 MB）"的技术债（TD-001）。

**下一步 M1**：APK 缩略图最小闭环——先出可编译工程 + `register.ps1`，用本机 APK 跑出第一组 ProcMon 与耗时数字，再谈属性页。

**下次开工清单（M1，按顺序）**

1. `CMakeLists.txt` + `src/thumbs` 空壳，能编出 x64 `apkthumb.dll`（`/MT`、`/W4 /WX`）。
2. `tools\register.ps1 -Install|-Uninstall`：写 `.apk\ShellEx\{E357FCCD…}` + CLSID，卸载可回滚。
3. ZIP 中央目录 + 最小 AXML + 最小 ARSC（TD-001 允许整读 ≤ 8 MB）→ WIC 出图。
4. 自建缓存 + 负缓存 + 300 ms 超时。
5. `tests\fixtures\` 三个自造包；跑 V1（ProcMon 0 命中）与 V2/V3（冷热耗时）并写进 `docs/verification.md`。

## 变更纪律（写给自己）

1. 每条技术断言要能指到出处：实测（二进制/注册表/ProcMon）、官方文档或对方源码。文档里不写"据说"。
2. 任何影响调用频率或进程边界的改动，先写 ADR 再改代码。
3. 每阶段结束在 `docs/verification.md` 追加一条记录（含命令与结论），未验证项单列，不许含糊。
