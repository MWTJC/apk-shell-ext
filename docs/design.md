# 设计出发点

> 这份文档回答"为什么是这个形态"，不回答"怎么写"（后者见 [architecture.md](architecture.md)）。
> 文中每条技术断言都标注了出处：`[实测]` = 本机二进制/注册表/工具输出；`[上游]` = 官方文档或项目 issue/源码。

## 1. 问题

Windows 不认 `.apk`（以及 `.aab`/`.ipa`/`.hap`/`.apks` 等）：资源管理器给白纸图标，属性页只有「常规」「安全」两页。开发者/玩家手里动辄几十上百个包，靠文件名猜版本、猜是哪个应用，成本很高。

已有工具只解决一半：

- **ApkShellext2**（kkguo，707★）解决图标，但 2019 年后停更，issue #120「It doesn't work on Windows 11」至今 open；且**没有属性页**（issue #110 自 2022-03 提出"加一个文件属性标签页"，至今 open、零回复）`[上游]`。
- **APK-Info**（Enyby，509★）信息很全（含 MD2–SHA512 全家桶），但只有独立窗口，壳集成仅是"用 APK-Info 打开"。
- **HashTab** 是"属性页形态"的范本，但只算哈希，与安装包无关。
- **果核看图** 证明了"缩略图 + 自己进程看信息"这条路在商业产品里可行（见 §5）。

所以本项目的价值不在"能算哈希"或"能读包名"，而在**把两件事按成本分到两个时机**：画图时机只付"几十 KB IO"的代价，看信息时机才付"全量解析"的代价。

## 2. 目标行为（用户可见的三件事）

| # | 行为 | 时机 | 允许的最大代价 |
| --- | --- | --- | --- |
| B1 | 大图标/超大图标/内容视图显示包内 launcher 图标 | Explorer 请求缩略图且缓存未命中 | ≤ 64 KB 读取、≤ 300 ms；超时回落通用图标 |
| B2 | 普通浏览（详细信息/列表/排序/悬停）零额外开销 | 每次进目录 | **0 次对 `*.apk` 的 CreateFile** |
| B3 | 右键 → 属性 → 「安装包信息」页，字段渐进填充 | 用户主动打开 | 页面骨架 ≤ 150 ms，全字段 ≤ 1 s |

B2 是本项目的硬约束，也是与所有既有实现的分水岭。

### 2.1 图标策略（2026-11-03 已确认）

| 视图 | 显示什么 | 机制 | 额外调用成本 |
| --- | --- | --- | --- |
| 大图标 / 超大图标 / 内容 | **包内 launcher 图标** | `IThumbnailProvider` | 首次 ≤ 64 KB IO；之后走缓存 |
| 详细信息 / 列表 / 小图标 | **格式差异化静态图标**（APK / XAPK / HAP 各一枚） | 每个扩展名的 `DefaultIcon`，纯注册表 | **0**（不进入任何代码路径） |

这是 ADR-0001 的配套决定：**放弃"小图标也显示包内图标"**，改用静态图标区分格式——既保留"一眼看出是安卓包 / 苹果包 / 鸿蒙包"的信息量，又不引入 `IExtractIcon` 的每帧调用。图标资源在 M5 出（多尺寸 ico：16/24/32/48/256），M1 阶段小图标仍是系统默认白纸。

## 3. 设计出发点

### P1 调用频率决定接口选择

Windows 给了四个可以"插手"的位置，频率差两个数量级：

| 接口 | 注册位置 | 谁在什么时候调 | 频率 | 可缓存 | 本项目 |
| --- | --- | --- | --- | --- | --- |
| `IThumbnailProvider` | `{E357FCCD-A995-4576-B01F-234630154E96}` | 只在大图标/内容视图且 thumbcache 未命中 | 每文件一生几次 | 系统 thumbcache + 自建 | ✅ 采用 |
| `IExtractIcon` | `{BB2E617C-0920-11D1-9A0B-00C04FC2D6C1}` | **每次绘制**（滚动/刷新/小图标/列表） | 每帧级 | 只能自己缓存 | ❌ v1 不用（M5 评估） |
| `IPropertyStore`（属性处理器/列） | `{8895B1C6-B41F-4C1C-A562-0D564250836F}` | 详情列、排序、悬停提示、预览窗格，**按目录批量** | 每次浏览 O(n) | 无 | ⛔ 禁止 |
| `IShellPropSheetExt` | `...\shellex\PropertySheetHandlers` | 用户打开属性对话框 | 一生几次 | 可自建 | ✅ 采用 |

**结论**：只要不注册 `IPropertyStore`，B2 天然成立——因为系统没有任何机会在浏览时叫我们。
这也是为什么"属性页"必须走 `IShellPropSheetExt`，而不是"往详细信息页塞字段"：后者看起来更原生，代价是把解析挂到每次浏览上。

### P2 进程边界：explorer.exe 里只放薄壳

属性页与缩略图 handler 都是 **in-proc COM**，DLL 直接进 `explorer.exe` 地址空间 `[实测]`（HashTab 的 `HashTab64.dll` 就是 `ThreadingModel=Apartment` 的进程内 server；果核看图的 `guohe-view-thumb.dll` 同样）。后果：

- handler 崩 → 桌面/资源管理器崩；
- DLL 的依赖全部变成 explorer 的依赖（缺 VC 运行库 = 整个功能静默失效）；
- DLL 里的大对象常驻 explorer 内存。

因此三条硬规定：

1. **静态链接 CRT（/MT）**，只允许系统 DLL：`kernel32/user32/shell32/shlwapi/ole32/oleaut32/gdi32/oleacc/windowscodecs`。禁止 MFC / WTL 之外的 UI 框架 / Qt / .NET。
2. **解析代码不进 explorer**：AXML/ARSC/签名/DEX/plist 全部在 `apkinfo.exe` worker 里；DLL 只做"定位 + 解码一张图 + IPC"。
3. **DLL 侧永不抛出跨边界异常**：`AddPages`/`GetThumbnail` 全部 `try/catch(...)` 兜底并返回失败码。

> 反例参照：果核看图的 thumb dll 体积只有 364 KB，但它在 explorer 进程里 `LoadLibrary("ghde.dll")`（6.9 MB 解码器）`[实测]`，且动态链接 `MSVCP140.dll` `[实测]`——能用，但把"依赖运行库"和"大模块进桌面进程"两个风险都背上了。我们只在 worker 进程里做等价的事。

> **M1 修订（2026-11-03，证据见 [verification.md R-0002](verification.md)、[ADR-0005](adr/0005-isolated-thumbnail-host-and-stream-input.md)）**：
> 上面"handler 直接进 explorer 地址空间"这条对**属性页**成立，对**缩略图**不成立——
> Windows 把缩略图 handler 放进 `dllhost.exe`（COM surrogate）跑，并且只通过
> `IInitializeWithStream` 喂数据（只实现 `IInitializeWithItem` 时系统直接返回
> `0x8004B200`，我们的代码根本不执行）。这比原判断更安全（崩的是 dllhost），
> 但要求实现按"流"工作，而不是按"路径"工作。三条硬规定（静态 CRT / 解析不进 explorer /
> 异常不跨边界）不变，其中第 2 条在缩略图路径上的含义是：DLL 内只做"中央目录 + 单条目解码"，
> 全量解析仍在 worker（M2）。

### P3 冷热分离：缩略图只碰中央目录与图标条目

缩略图请求允许做的最小工作：

1. 读 ZIP **中央目录**（EOGZ + CD，典型 APK 几十 KB）；
2. 从 `AndroidManifest.xml`（二进制 AXML）取 `application@android:icon` → 资源 ID；
3. 在 `resources.arsc` 里按 ID 找最佳密度变体（或 adaptive icon 的图层）；
4. 只 seek 读该条目的压缩数据并解码（PNG/WebP，WIC 原生支持）。

**不**做的事：读 DEX、算签名指纹、解析权限表、遍历全部 locale、算 SHA-256（哈希只在属性页算，且按需）。

风险点与对策：

- `resources.arsc` 可能几 MB，不能整读 → 只解析 `RES_TABLE` 头 + `RES_STRING_POOL` + 目标 `RES_TABLE_PACKAGE` 的相关 chunk，其余跳过；
- adaptive icon（`mipmap-anydpi-v26/ic_launcher.xml`）本身是 XML，需合成 foreground/background 两层 → M1 先只取 PNG/WebP 变体，合成放 M2；
- 加固/混淆包条目名异常 → 用中央目录 ordinal 定位，不信任条目名字符集。

### P4 元数据全量只在属性页，且必须异步

`IShellPropSheetExt::AddPages` 在 **explorer 的 UI 线程**上执行，属性对话框是**模态**的 `[上游]`。同步读 APK = 桌面假死，几百 MB 的包会假死十几秒。

规定：

- `AddPages` 只做 `CreatePropertySheetPage` + 返回，耗时 < 50 ms；
- 页面创建后 `PostMessage` 起任务（线程或 worker 进程），先渲染"解析中…"骨架；
- 结果回填通过 `WM_APP+n` 消息，页面自己判断是否已销毁；
- 用户中途关窗：取消 worker（`TerminateProcess` 兜底），不等待、不弹错。

### P5 「无感」必须可度量

B2 不能靠感觉，写成工具判据（[verification.md](verification.md)）：

- ProcMon 过滤 `Operation is CreateFile` + `Path ends with .apk`：详细信息/列表视图滚动 30 s → **0 命中**；
- 大图标视图首次进入含 100 个 APK 的目录 → 每个包读取 ≤ 64 KB；
- 属性页冷开 ≤ 1 s、热开（缓存命中）≤ 200 ms；
- explorer 私有提交增量 ≤ 5 MB。

## 4. 关键决策

| 决策 | 结论 | 理由 | ADR |
| --- | --- | --- | --- |
| 缩略图接口 | 只挂 `IThumbnailProvider`，v1 不挂 `IExtractIcon` | 小图标视图不需要包内图标；多挂一个就是每帧一次调用 | [0001](adr/0001-thumbnail-provider-only.md) |
| 元数据入口 | 只注册 `IShellPropSheetExt`；**禁止** `IPropertyStore`/列/预览处理器 | B2 的唯一保证 | [0002](adr/0002-lazy-metadata-and-banned-handlers.md) |
| 解析位置 | 独立 worker 进程 `apkinfo.exe --json` | 崩溃隔离、依赖隔离、explorer 不背解析器 | [0003](adr/0003-out-of-proc-parser.md) |
| 技术栈 | C++20 / MSVC v143 / CMake / 静态 CRT / Win32+WTL | 无托管运行时、无 redist 依赖、体积小 | [0004](adr/0004-stack-and-dependency-boundary.md) |

## 5. 既有方案实测（为什么不是"照抄一个"）

### 5.1 HashTab 5.1.0.23（本机 `E:\PROGRAMS\HashTab …\HashTab64.dll`，739,840 B）`[实测]`

| 结论 | 证据 |
| --- | --- |
| C++ / MSVC v10.0，2013-01 编译 | PE `LinkerVersion=10.0`；无 CLR 目录 |
| ATL COM，双接口 | 导出仅 `DllGetClassObject/CanUnloadNow/DllInstall/DllRegisterServer/DllUnregisterServer`；RTTI `CComObject<CHashPage>`、`IDispatchImpl<IHashPage>`；资源含 TYPELIB |
| WTL 对话框 | RTTI `CDialogImplBaseT<CPropertyPageWindow>`、`CFileDialogImpl`；2 个 DIALOG 模板 |
| `IShellPropSheetExt` | RTTI `.?AUIShellPropSheetExt@@`；注册在 `HKCR\*\shellex\PropertySheetHandlers\HashTab`，`ThreadingModel=Apartment` |
| 静态 CRT + Crypto++ | 导入仅 8 个系统 DLL，无 `msvcr*/vcruntime*`；RTTI `CryptoPP::Weak1::MD5`、`IteratedHashBase`、`Whirlpool`、`RIPEMD320` |

**可借的**：薄、静态 CRT、Apartment、属性页用对话框模板；**不该借的**：它自己就在"每次属性页打开"时同步算哈希（大文件会卡），我们用 worker + 渐进填充。

### 5.2 果核看图 3.4.16.125（本机 `E:\PROGRAMS\GuoheView`）`[实测]`

| 结论 | 证据 |
| --- | --- |
| 全栈 C++，非 .NET | `GuoheView.exe` 4.1 MB，linker 14.0，导入 `MSVCP140/VCRUNTIME140`，CLR 目录为空 |
| 自研 CORE UI（Direct2D/DirectWrite 路线） | `core-ui.dll` 4.8 MB；exe 内嵌 HTML/CSS 片段与 `ui_*` C API，`bind_widgets_` 做 C++ 绑定 |
| 解码器独立成 DLL | `ghde.dll` 6.9 MB，导出 `ghde_decode_thumbnail` 等 |
| **唯一的壳扩展只做缩略图** | `guohe-view-thumb.dll` 364 KB，x64，导出 4 个标准 COM 函数，RTTI 仅 `GuoheThumbProvider` + `IThumbnailProvider`；**无 `IShellPropSheetExt`、无 `IExtractIcon`** |
| 按需加载大模块 | thumb dll 运行时 `LoadLibrary("ghde.dll")` 取 `ghde_decode_thumbnail_from_reader` |
| 注册范围 | 仅 10 个图片扩展的 `{E357FCCD…}`，全部指向同一 CLSID；**未注册任何 PropertySheetHandlers** |

**结论**：一个商业看图软件在 shell 侧也只放"最小缩略图 provider"，其余信息留在自己进程里——与本项目的 B1/B2 边界一致。它没有属性页；本项目多出来的正是 B3。

### 5.3 ApkShellext2 / ApkShellext2-Plus `[上游]`

- 上游 `.NET Framework 4.5/4.7.2 + SharpShell`，2019 年后停更；issue #120 报 Win11 失效；内置 libwebp 未修 **CVE-2023-4863**（issue 仍 open）——恶意 APK 的图标即可触发，这是"不推荐继续用"的实质理由。
- `ApkShellext2-Plus`（MaterDawn，2026-10）修了混淆包/毒包/鸿蒙 hap，但**仍只有 Infotip，没有属性页**，且**无 release**，需自行 MSBuild。
- 两者都用 Infotip（悬停）承载元数据：本质仍是"每次悬停都解析"，与 P1 冲突；我们不用。

## 6. 性能模型（量级估算，M1 用实测替换）

目录 500 个 APK，全部从未浏览过：

| 场景 | 本方案 | ApkShellext2 类（Infotip + icon） |
| --- | --- | --- |
| 详细信息视图滚动 30 s | 0 次打开 | 每次悬停 1 次解析（含 AXML/ARSC） |
| 切到大图标视图 | 500 × (CD ~20 KB + icon ~50 KB) ≈ 35 MB 一次性 IO，之后走 thumbcache | 同上，且无跨进程缓存 |
| 右键属性 1 个包 | 50–300 ms 解析 + 50 ms 页面 | 无此功能 |

## 7. 非目标与禁止项（M6 前不得以"体验更好"为由突破）

1. 不注册 `*`（所有文件）级 handler，只注册安装包扩展名。
2. 不注册 `IPropertyStore`、列处理器、预览处理器、`IExtractIcon`（v1）。
3. 不在 `AddPages`/`GetThumbnail`/`Initialize` 里做同步磁盘全量解析。
4. 不在 explorer 进程里加载任何第三方大库（> 1 MB）或托管运行时。
5. 不联网、不写 APK、不自动更新（更新由 installer 负责）。
6. 不把 APK 内容解压到临时目录做"顺便落盘"（只流式读，缓存仅存我们自己生成的缩略图/JSON）。

## 8. 未决问题（M5 已决的标注结论）

- ~~差异化 ico 的具体视觉~~ —— 已决（M5；2026-11-04 改版 v2）：**一套"纸页 + 色带"语言，颜色分族**——
  `.apk` 与多 APK 容器同用安卓绿（单页 / 两页区分），`.hap` 红色；不带版本角标；16/24/32/48/256 各档独立绘制。
  见 [m5-delivery.md](m5-delivery.md) §2 与 [verification.md R-0015](verification.md)。
- 缩略图是否叠加"类型角标"（APK/IPA/HAP 小标记），还是保持纯净。
- ~~MSIX/APPX 是否纳入~~ —— 已决：**不做**（[ADR-0006](adr/0006-format-scope-cut.md)，2026-11-04）。
- 是否购买代码签名证书（EV/OV）：影响 SmartScreen 与杀软误报率；APK-Info 至今仍有误报 `[上游]`。
- ~~缩略图缓存是否允许用户"按目录清理"~~ —— 已决（M5）：**不做**按目录清理，只提供全局
  `apkinfo --clear-cache` / `--cache-info`；缓存键含解析器版本，升级即整体失效。
- 安装形态（自研 vs 现成安装器、按用户 vs 整机）—— 已决（M5）：[ADR-0007](adr/0007-inno-setup-per-user-install.md)。
