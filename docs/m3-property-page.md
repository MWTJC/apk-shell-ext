# M3 · 属性页（`apkprop.dll`）设计方案

> 状态：**已评审、已实现（2026-11-04）**。§12 的四项决策见文末"决策记录"，实现与实测见 §14。
> 相关：[architecture.md §2.2/§4](architecture.md)、[roadmap.md · M3](roadmap.md)、[ADR-0003](adr/0003-out-of-proc-parser.md)、[design.md · P4](design.md)
> 本文不新增 ADR：**进程边界与调用频率都不变**（worker 仍是每次调用一个进程、属性页仍只在"打开属性"时触发），
> 按 project 纪律属于"不需要 ADR"的改动。

## 0. 一句话

> 属性页在 `AddPages` 里**不碰磁盘**；`PSM_INITDIALOG` 之后起线程 → 起 `apkinfo.exe` → 读 JSON → `WM_APP` 回填。
> UI 线程只负责画和复制，任何解析失败都变成**页面里的一行字**，而不是 explorer 的一次异常。

## 1. 范围

**做**

1. 单页「安装包信息」，形态参照 HashTab：顶部标识区 + 4 个字段组 + 底部状态区。
2. 字段全部来自 `apkinfo.exe --json`（schema v1），页面不自己解析 APK（唯一例外：`GetFileAttributesExW` 取大小/时间，成本 ~0.1 ms）。
3. 错误态、取消、深色模式、高 DPI、逐行复制。

**不做（v1）**

- 多页 / 多标签、字段可编辑、"另存为 JSON 文件"（只提供复制到剪贴板）。
- 签名校验（"签名是否有效"需要重算摘要，见 [architecture.md §2.3](architecture.md) 的 `signature` 语义）。
- 常驻守护进程（ADR-0003 已否）。
- 缩略图之外的新字段（如 DEX 方法数）：需要改 schema 与解析核心，另开阶段。

## 2. 布局（冻结）

页面尺寸 **420 × 470 DIP**（属性对话框常见宽度；150% DPI 下 630 × 705 px，屏幕放得下）。

```
┌──────────────────────────────────────────────────────────┐
│  ┌──────┐  应用名（label，缺省回落文件名）                │
│  │ 48×48│  包名 com.example.app                         │
│  │ 图标 │  版本 1.2.3 (10203) · minSdk 26 / targetSdk 34 │
│  └──────┘                                                │
├──────────────────────────────────────────────────────────┤
│ 应用                                                     │
│   应用名      PNG Icon Demo                              │
│   包名        com.example.pngicon                        │
│   版本        1.2.3 (10203)                              │
│   语言        zh-CN, en                                  │
├──────────────────────────────────────────────────────────┤
│ 技术                                                     │
│   ABI         arm64-v8a, armeabi-v7a                     │
│   密度        xxxhdpi, xxhdpi, xhdpi, hdpi, mdpi         │
│   组件        活动 12 · 服务 3 · 广播 2 · 提供者 1        │
│   图标来源    res/mipmap-xxxhdpi-v4/ic_launcher.png      │
├──────────────────────────────────────────────────────────┤
│ 权限与组件                                               │
│   权限        6 项  ▸（点击展开列表）                     │
│   特性        android.hardware.camera（必需）…           │
├──────────────────────────────────────────────────────────┤
│ 文件与签名                                               │
│   大小        8.7 KB                                     │
│   修改时间    2026-11-04 00:55:30                        │
│   SHA-256     — [计算]                                   │
│   签名        v1, v2, v3                                 │
│   证书        CN=Android, O=Android, C=US                │
│   证书摘要    a40da80a…（完整值可复制）                   │
│   有效期      2008-02-29 → 2035-07-17                    │
├──────────────────────────────────────────────────────────┤
│ 解析中…  103 ms              [复制全部为 JSON]  [重试]    │
└──────────────────────────────────────────────────────────┘
```

**冻结规则**

1. 组顺序与组内行顺序固定（上表即 v1 定稿），之后**只增不删、只往后追加**。
2. **没有值的行不显示**（不是显示"—"），避免大段空白；展开后不重排，只在缺失/出现时整页重算一次。
3. 长值一律单行省略号截断，悬停显示全文（tooltip），复制拿全文。
4. 复制粒度：行右键 →「复制值」/「复制 `键: 值`」；`Ctrl+C` = 复制当前选中行；底栏按钮 = 原始 JSON。

## 3. 控件选型（决策 A，待拍板）

| 方案 | 深色 | 高 DPI | 复制/选择 | 无障碍 | 预计代码量 |
| --- | --- | --- | --- | --- | --- |
| A1 全自绘（**推荐**） | 完全可控 | 完全可控 | 自己实现（命中测试 + 剪贴板） | 弱（只做 `WM_GETOBJECT` 之外的最小提示） | ~600 行 |
| A2 `SysListView32` report + ownerdraw | 需要 `SetWindowTheme("DarkMode_Explorer")`，表头/边框仍有白边 | 可 | 现成（多选、Ctrl+C 要自己挂） | 好 | ~400 行 + 主题补丁 |
| A3 标准 Static + 只读 Edit 拼装 | 每个控件都要 `WM_CTLCOLORSTATIC` 单独处理，滚动要自己做 | 可 | 现成 | 好 | ~350 行但观感最差 |

**推荐 A1**：本页本质是"一张两列表格"，自绘一次投入换来深色/DPI/行高/分组标题全部一致；
HashTab 也是自绘列表，用户对"属性页里能选中复制"的预期它已经立好了。代价是无障碍弱，记为已知限制。
**逃生舱**：如果自绘超过 600 行还有毛刺，退回 A2（`LVS_OWNERDRAWFIXED` + 自绘滚动条）。

## 4. 线程与生命周期

```
AddPages(pf, lParam)                       // explorer UI 线程，模态对话框创建前
  └ CreatePropertySheetPage(&PROPSHEETPAGE{ pfnDlgProc, pszTemplate, lParam=ctx })
  └ return TRUE                            // 目标 ≤ 50 ms，零 IO

WM_INITDIALOG                              // 页面对话框初始化，仍是 UI 线程
  ├ 骨架布局（只画组标题 + "正在解析…"）
  ├ 读文件大小/时间（GetFileAttributesExW，~0.1 ms）
  ├ CreateThread(WorkerProc, ctx)          // 独立线程，非 UI 线程池
  └ return FALSE/TRUE（不自设焦点）

WorkerProc（后台线程）
  ├ 组命令行 → CreateProcessW(apkinfo.exe) 并加入 Job（KILL_ON_JOB_CLOSE）
  ├ 读 stdout 到上限 4 MiB（可中断）→ 等进程退出（超时 = 8 s + 2 s 宽限）
  ├ 退出码 + JSON → PageModel（在这里解析 JSON，不在 UI 线程）
  └ PostMessage(hwnd, WM_APP_FILLED, 0, (LPARAM)model)   // ctx->hwnd 判空后再发

WM_APP_FILLED                              // UI 线程
  ├ 换模型 → 重算行高/滚动范围 → InvalidateRect
  └ 释放旧模型（模型所有权在页面，消息里只传指针）
```

**取消与竞态（这是 M3 最容易写错的地方）**

1. `PKN_DESTROY`/`WM_DESTROY`：置 `ctx->cancelled = true` → `TerminateJobObject(job)`（连子进程一起杀）→ `ctx->hwnd = nullptr`（与 `PostMessage` 用同一个 `SRWLOCK` 保护）。
2. 线程发现 `cancelled` 后直接退出，**不碰 HWND、不碰模型**；Job/进程/管道句柄由线程自己 `CloseHandle`。
3. 关窗后 200 ms 内 `apkinfo.exe` 必须消失；验收里专门测"解析中关窗 ×20 次后 `Get-Process apkinfo` 为空"。
4. `ctx` 用 `shared_ptr` 由 UI 与线程共同持有，谁最后走谁释放（不 join 线程，避免 UI 卡住）。
5. 同一页面只允许一个 worker；重复 `WM_APP_START`（例如"重试"）先取消旧的再起新的。

**句柄预算**：job / process / thread / 2 × pipe / stdout buffer 文件映射都不需要 —— 单次 ≤ 6 个；全部由线程在退出路径关闭。

## 5. worker 调用与数据契约

| 项 | 约定 |
| --- | --- |
| 命令行 | `"<DllDir>\apkinfo.exe" --json --schema=1 --timeout=8 "<path>"`，`CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT` |
| exe 路径 | `GetModuleFileNameW(apkprop.dll)` + 去掉文件名（**不走 PATH**，避免被同名 exe 劫持） |
| 转义 | 按 `CommandLineToArgvW` 的规则给路径加引号并转义 `\` 与 `"`（单独写 `QuoteArg()` 并有单测） |
| stdout | 全部读完才算成功；上限 **4 MiB**（正常输出 < 64 KiB），超限即终止并报错 |
| 超时 | 传给 worker 8 s（watchdog 自杀，退出码 3），页面侧等 10 s 兜底 |
| schema | 只认 `schema == 1`；更大 → "解析器版本不匹配（需要更新扩展）" |
| stderr | 只在诊断开关打开时写 `%LOCALAPPDATA%\apk-shell-ext\apkprop.log`，不解析 |
| 退出码 | 0 成功 / 2 不是有效安装包 / 3 超时 / 其它或异常码（`0xC0000005` 等）= 崩溃 |

## 6. 错误态矩阵

| 场景 | 页面表现 | 「复制诊断」内容 |
| --- | --- | --- |
| 未安装 worker / 文件缺失 | 「解析器缺失：`<路径>`」+ [重试] | 路径、`GetLastError` |
| 退出码 2（不是安装包） | worker 的 stderr 第一行（如「包内缺少 AndroidManifest.xml」） | 路径、命令行、stderr 全文 |
| 退出码 3 / 页面超时 | 「解析超时（8 s）」+ [重试] | 已等待时长、文件大小 |
| 进程崩溃 | 「解析器异常退出（0xC0000005）」+ [重试] | 退出码、命令行、stderr |
| 输出超限 / JSON 截断 | 「解析输出异常」 | stdout 前 1 KiB（十六进制安全转义） |
| 非 `.apk` 的其它格式（M4 之前） | 「暂不支持的格式：`.ipa`」 | 扩展名 |
| 解析中关窗 | **静默**取消，不留任何弹窗 | — |

**是否总是加页**：是。`AddPages` 一律添加（无法提前知道包是否有效），错误也显示在页内 —— 让用户知道
"这里有个能解析的扩展，只是这个文件不行"，比"没有这个标签页"更可解释。

## 7. 深色模式 / 高 DPI / 字体

| 项 | 做法 |
| --- | --- |
| 主题判定 | 读 `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`；订阅 `WM_SETTINGCHANGE`（`ImmersiveColorSet`）后重画 |
| 颜色 | 只用 6 个令牌：背景、分组标题背景、主文本、次文本、分隔线、链接/按钮（亮/暗各一套，写死在 `theme.cpp`） |
| DPI | `GetDpiForWindow` + `WM_DPICHANGED` 重算；所有尺寸写 DIP，统一 `Scale(dpi, dip)`；行高 = `fontLineHeight + 8 DIP` |
| 字体 | `SystemParametersInfoW(SPI_GETNONCLIENTMETRICS)` 的 `lfMessageFont`，按 dpi 建 `HFONT`；**不硬编码 "Segoe UI 9"** |
| 图标 | 优先读缩略图缓存 PNG（命中就是最终图标），未命中用安装目录的 `icons\apk.ico` 兜底 —— 属性页**绝不**为出图标去解析 APK |

## 8. 性能预算与测量

| 判据 | 目标 | 怎么测 |
| --- | --- | --- |
| `AddPages` | ≤ 50 ms | 诊断日志打点（`AddPages` 入口/出口各一条，`QPC` 时间戳） |
| 冷开（含进程启动） | P95 ≤ 1 s | 20 次「打开属性 → 关闭」，日志里 `t_filled - t_addpages` |
| 500 MB 包 | ≤ 3 s | 用手边的 GB 级包，日志同上 |
| 热开 | ≤ 200 ms | 同上（同一文件二次打开；如果 v1 不加 meta 缓存，这一条等于"worker 本身够快"） |
| 关窗取消 | ≤ 200 ms 内无 `apkinfo.exe` | `Get-Process apkinfo` 轮询 + 日志 |
| 句柄 | 100 次开关回到基线（±2） | `procexp`/`Get-Process explorer | Select HandleCount` |

**预估**：进程启动 20–40 ms（ADR-0003 的代价）+ 解析 10–12 ms（R-0005 实测）+ 布局 < 5 ms ⇒ 冷开 ~60 ms，
预算余量很大；真正的变量是杀软对 `CreateProcess` 的拦截，验收里要记录实际值。

## 9. 注册表与注册脚本

```
HKCR\.apk\shellex\PropertySheetHandlers\ApkInfo   (default) = {PropCLSID}
HKCR\CLSID\{PropCLSID}                            (default) = "APK 安装包信息"
HKCR\CLSID\{PropCLSID}\InprocServer32             (default) = <install dir>\apkprop.dll
                                                  ThreadingModel = Apartment
```

- `PropCLSID`：`{6F1D3A42-8C5B-4E97-A2D3-9B7C41E5F806}`（可换，但一旦发布不能改：卸载残留会认不出）。
- `tools/register.ps1` 增加 `-Feature All|Thumb|Propsheet`：沿用现有的"备份 → 写 → 失败回滚"机制；
  `-Uninstall` 必须把 `PropertySheetHandlers\ApkInfo` 整个键删掉（V6 回归：注册表 diff = 0）。
- 与缩略图共用 DLL 目录解析逻辑；`.apk` 以外的扩展名等 M4 再逐个子键注册。

## 10. 验收（对应 roadmap M3 六条 + 三条新增）

| # | 判据 | 方法 |
| --- | --- | --- |
| M3-1 | 冷开 P95 ≤ 1 s，500 MB ≤ 3 s，热开 ≤ 200 ms | §8 的日志打点，20 次 |
| M3-2 | 解析中关窗 × 20：不崩、不残留 `apkinfo.exe`、句柄回基线 | 脚本 + `Get-Process` |
| M3-3 | 连续开关 100 次：explorer 句柄/私有提交无单调增长 | `Get-Process explorer` 采样 + 线性回归斜率 |
| M3-4 | 150% / 200% DPI 不裁切、不重叠 | 手动截图（PNG 入库 `docs/evidence/`） |
| M3-5 | 深色模式文字对比度 ≥ 4.5:1（次文本 ≥ 3:1） | 取色计算，记录数值 |
| M3-6 | worker 人为 abort：页面显示「解析器异常退出」，explorer 存活 | `APKSX_TEST_ABORT_MS`（新增测试钩子，语义同 `APKSX_TEST_DELAY_MS`） |
| 新增 1 | 页面字段与 `apkinfo --json` 逐字段一致（3 真实包 + 2 损坏包） | 人工对照表 + 截图 |
| 新增 2 | 中文/英文系统语言下文案不缺字（v1 只做中文 + 英文两套 .rc） | 切换语言手动看 |
| 新增 3 | 属性页打开时 ProcMon 只出现 1 次 `CreateFile *.apk`（worker 那次），explorer 进程 0 次 | 复用 V1 配方，过滤 `explorer.exe` |

## 11. 交付物与工作量

```
src/propsheet/
├─ dllmain.cpp          DllMain / DllGetClassObject / DllCanUnloadNow
├─ class_factory.cpp    IClassFactory（只产出 PropertyPage 对象）
├─ prop_page.cpp        IShellPropSheetExt + IPersistFile + 页面生命周期状态机
├─ worker_client.cpp    QuoteArg / CreateProcessW / Job / 读管道 / 超时 / 取消
├─ page_model.{h,cpp}   JSON → 行模型（不含 UI 依赖，可单测）
├─ page_paint.cpp       自绘（分组、行、省略号、滚动条、命中测试）
└─ theme.{h,cpp}        6 个颜色令牌 + 字体 + DPI 缩放
tests/propsheet_model_test.cpp   直接驱动 page_model（无 GUI），用现有夹具的 JSON 断言
```

估时 **2–3 人日**：UI/自绘 1 d、worker_client 0.5 d、错误态 + 深色/DPI 0.5 d、测试与文档 0.5–1 d。

## 12. 待拍板

| # | 问题 | 选项 | 我的建议 |
| --- | --- | --- | --- |
| A | 列表实现 | 自绘 / ListView ownerdraw / 标准控件 | **自绘**（深色与 DPI 一致，代价是无障碍弱） |
| B | SHA-256 | 按需按钮 / 自动算 / 不显示 | **按需按钮**（1.9 GB 包要 1.16 s，自动算会让"冷开 ≤1s"随包大小失效） |
| C | 是否引入 meta JSON 缓存 | 不引入 / 引入（`%LOCALAPPDATA%\apk-shell-ext\meta\`） | **v1 不引入**：解析只要 10 ms，缓存反而带来"钟表不准"的一致性问题；等 M4 有了慢格式（AAB/IPA）再说 |
| D | 页面宽度/密度 | 420 DIP 宽 + 只显示有值的行 / 更宽 + 固定行 | **420 DIP + 只显示有值的行** |

## 13. 风险

| # | 风险 | 应对 |
| --- | --- | --- |
| P1 | 自绘列表无障碍差（Narrator 读不到） | 记为已知限制；至少给整页 `WM_GETOBJECT` 返回 `ROLE_SYSTEM_PANE` + 文本摘要 |
| P2 | 150% DPI 下长包名/长证书主题溢出 | 单行省略 + tooltip + 复制全文；DPI 用例覆盖 200% |
| P3 | explorer 的深色接口在不同 build 上表现不一致 | 只依赖公开的 `AppsUseLightTheme` 注册表值 + 自绘，不调用未公开的 `SetPreferredAppMode` |
| P4 | 杀软拦截 `CreateProcess` 使冷开变慢/失败 | 错误态明确提示；M5 代码签名后复测 |
| P5 | 属性页 handler 在 explorer 里崩溃 | 全部入口 `try/catch`；解析在 worker；`/guard:cf` 已有 |

## 14. 实施记录（2026-11-04）

### 落地清单

```
src/propsheet/
├─ dllmain.cpp / class_factory.cpp   CLSID {A7C4E1B0-…-6D1C8E4A7B02}、DllGetClassObject/CanUnloadNow
├─ prop_page.cpp                      IShellPropSheetExt + IPersistFile + IShellExtInit、线程、取消、剪贴板
├─ worker_client.{h,cpp}             QuoteArg / CreateProcessW / Job(KILL_ON_JOB_CLOSE) / 管道 / 超时
├─ page_model.{h,cpp}                JSON → 行模型（wchar_t，无 UI 依赖，可单测）
├─ page_view.{h,cpp}                 布局 + 全自绘（组标题/行/子项/省略号/底栏按钮/命中测试）
├─ theme.{h,cpp}                     6 个配色令牌 + SPI 字体 + DPI
└─ apkprop.{rc,def,h}                对话框模板（无控件）、字符串、VERSIONINFO、导出表
src/core/json_lite.{h,cpp}           最小 JSON 解析（进 explorer 的代码不引第三方）
tests/propsheet_model_test.cpp       行模型单测（26 条断言，不需要窗口）
tests/proptest.cpp                   显式加载 DLL → AddPages → PropertySheetW，可直接截图
tools/capture-propsheet.ps1          打开属性对话框 → 选标签 → 可选点击/滚轮 → 只截该窗口
```

### 计划外发现（必须记住）

**`IShellExtInit` 是硬要求。** 实测日志（本机 Windows 11 `10.0.26300`）：

```
QI(IShellPropSheetExt) → CreateInstance → QI(IShellPropSheetExt) → QI(IShellExtInit=000214E8)
```

在此之前，handler 会被创建、但 `IPersistFile::Load` 与 `AddPages` **一次都不会被调用**，属性页里也就没有这一页。
补上 `IShellExtInit::Initialize`（从 `CF_HDROP` 取第一个路径）之后，`Load → AddPages → 页面出现` 全通。
只按老文档实现 `IPersistFile` 的 handler，在这个 build 上就是"注册成功但不显示"。

**注册表不需要 `Approved` 列表、也不需要 `HKCR\*`。** 只写
`HKCR\.apk\shellex\PropertySheetHandlers\ApkInfo` + `HKCR\CLSID\{…}\InprocServer32`（HKCU 即可）就生效；
排查时临时加过 `\*` 与 `SystemFileAssociations`、以及 `Approved` 条目，逐一移除后仍正常，已全部撤销。

### 与方案的偏差

1. **头部图标加了"本地解一个条目"的退路**：缩略图缓存在 dllhost/stream 模式下用的键与"按路径"不同，
   属性页不一定命中。现在先查缓存，未命中就在 worker 线程里用 core 解一个图标条目（与 `apkthumb` 同一套代码，
   仍然只读中央目录 + 一个条目，不做全量解析）。图标解码失败就画占位方块。
2. **`PSM_INITDIALOG` 之后先画骨架**：label 用文件名兜底、组标题为"正在解析…"，避免空窗口。
3. 错误态的行用"空键"绘制（从左边距开始），不再占键列。

### 实测（本机，深色主题 100% DPI）

| 场景 | 结果 | 证据 |
| --- | --- | --- |
| `aida64-1.94.apk`（7.9 MB） | 解析完成 **140–188 ms**；哈希 **172 ms** | `docs/evidence/m3-propsheet-light.png`、`m3-propsheet-hash.png` |
| `base2_1.92G.apk`（1.79 GB） | 解析完成 **157 ms** | `docs/evidence/m3-propsheet-big.png` |
| 展开权限（18/6 项） | 行内展开，子项缩进 | `docs/evidence/m3-propsheet-expanded.png` |
| 损坏包（`notzip.apk` 改名） | 页内报"未找到 ZIP 结束记录"，带重试/复制 JSON | `docs/evidence/m3-propsheet-error.png` |
| 关闭时正在算哈希（1.92 GB × 3 次） | 关窗后 `Get-Process apkinfo` = 0，无残留 | 见 verification.md R-0006 |
| 行模型单测 | 26/26 | `propsheet_model_test --selftest` |

### 已知限制 / 未验证

1. 无障碍：自绘列表对 Narrator 不友好（§13 P1 已登记，v1 接受）。
2. 150% / 200% DPI、浅色主题未截图验证（当前机器是深色 100%）。
3. 100 次开关的句柄/私有提交趋势、`worker 人为 abort`（需要新增测试钩子）未做。
4. 中英文双语只做了中文文案；`.rc` 的 STRINGTABLE 留了 ID，英文在 M5 一起做。
