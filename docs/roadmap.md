# 实现阶段（M0–M5）

> 阶段划分原则：**每个阶段都能单独结束并留下可复现的数字**。没有"做到一半"的里程碑——
> 每阶段都写明"交付物 / 验收（可测）/ 退出条件"，未达标不进下一阶段。
> 估时按"一个人、有完整工具链、不含意外"估算，随实测修正。

## 0. 总览

```mermaid
flowchart LR
    M0["M0 文档与基线<br/>(当前)"] --> M1["M1 缩略图闭环<br/>仅 .apk"]
    M1 --> M2["M2 解析核心 + worker<br/>JSON v1"]
    M2 --> M3["M3 属性页<br/>异步填充"]
    M3 --> M4["M4 全格式<br/>APKS/AAB/IPA/HAP/APPX"]
    M4 --> M5["M5 交付打磨<br/>安装器/签名/回归"]
```

| 阶段 | 主题 | 产出 | 估时 | 状态 |
| --- | --- | --- | --- | --- |
| M0 | 文档与接口/验收基线 | 本套文档 + 4 条 ADR | 0.5 d | ✅ 完成（2026-11-03 评审通过） |
| M1 | APK 缩略图最小闭环 | `apkthumb.dll` + 注册脚本 + 缓存 | 3–4 d | ✅ 完成（2026-11-03，记录见 [verification.md R-0002](verification.md)） |
| M2 | 解析核心与 worker | `apkinfo.exe` + JSON schema v1 + 测试固件 | 2–3 d | ✅ 完成（2026-11-04）：JSON + aapt2 交叉校验、TD-001 关闭、签名块、features、`splits`（随 M4a）全部落地；仅剩 TD-002 与 V8 回归 |
| M3 | 属性页 | `apkprop.dll`（原生控件、异步、错误态、复制、可折叠分组） | 2–3 d | ✅ 用户已验收（[R-0006](verification.md)·[R-0009](verification.md)·[R-0010](verification.md)）；DPI 由布局单测覆盖（[R-0011](verification.md)） |
| M4 | 全格式 | APKS/XAPK/APKM → AAB → IPA → HAP → APPX | 6–9 d | 🟡 M4a ✅（.xapk/.apks/.apkm，[R-0011](verification.md)）· M4d ✅（.hap，[R-0012](verification.md)）；M4b/M4c/M4e 未开始 |
| M5 | 交付打磨 | 安装器、签名、i18n、诊断、回归 | 3–4 d | ⬜ |

**M0 评审记录（2026-11-03）**——三项决策已确认，后续实现不得再讨论：

1. **小图标/列表视图放弃包内图标**，改用按格式区分的静态 ico（`DefaultIcon`，零调用）。见 [design.md §2.1](design.md)、[architecture.md §2.4](architecture.md)。
2. **技术栈维持 C++ / 静态 CRT / 无托管运行时**（性能优先）。见 [ADR-0004](adr/0004-stack-and-dependency-boundary.md)。
3. **M1 接受技术债 TD-001**：先整读 `resources.arsc`（≤ 8 MB），M2 改分段流式并删除该路径。见 §5。

**总计约 17–24 人日**，其中真正的硬骨头是 M4b（AAB 的 protobuf）与 M4c/d（Assets.car、鸿蒙 resources.index）。

## M0 · 文档与基线（已完成）

**交付**

- `README.md`、`docs/design.md`、`docs/architecture.md`、`docs/formats.md`、`docs/roadmap.md`、`docs/verification.md`
- ADR-0001 ~ 0004（缩略图接口、禁止项、worker、技术栈）

**验收**

- 三条用户可见行为（B1/B2/B3）都有对应判据与测量方法；
- 注册表契约、JSON schema v1 草案、格式矩阵可直接照着实现；
- 禁止项清单明确（不注册 `IPropertyStore`、不在 `AddPages` 同步 IO 等）。

**退出条件**：文档评审通过（**已于 2026-11-03 通过**，三项决策确认见上方"评审记录"）；任何后续"想加列 / 想加悬停提示"的改动都必须先改 ADR。

## M1 · APK 缩略图最小闭环

**范围**：只支持 `.apk`，只出缩略图，不做属性页、不做元数据。

**交付**

1. CMake 工程（MSVC v143，x64，`/MT`，`/W4 /WX`）。
2. `apkthumb.dll`：`IThumbnailProvider` + `IInitializeWithItem`；EOCD/中央目录解析；最小 AXML（只到 `application@android:icon`）；最小 ARSC（字符串池 + 目标资源 ID 的 density 变体）；WIC 解码 PNG/WebP。
3. 自建缓存 + 负缓存 + 300 ms 软超时。
4. `tools\register.ps1 -Install|-Uninstall`（管理员），只写 `.apk`。
5. `tests\fixtures\` 里 3 个自造最小 APK（无图标 / PNG 图标 / WebP 图标）+ 1 个真实包。

**验收（必须逐条贴数字）**

| 项 | 判据 | 方法 |
| --- | --- | --- |
| V1 零开销 | 详细信息视图滚动 30 s，`CreateFile *.apk` = **0** | ProcMon 过滤（见 verification.md） |
| V2 冷缩略图 | 100 个 APK，P50 ≤ 120 ms，P95 ≤ 300 ms | 脚本遍历 + ETW/日志 |
| V3 热缩略图 | 同目录二次进入 ≤ 20 ms/个 | 同上（缓存命中） |
| V4 内存 | explorer 私有提交增量 ≤ 3 MB | VMMap / 任务管理器 |
| V5 健壮性 | 截断包/空文件/非 ZIP 改名 `.apk` 不崩 explorer | 手工样本 |
| V6 可卸载 | 卸载后注册表与安装前 diff = 0 | reg export 前后对比 |

**退出条件**：V1–V6 全绿，且"图标是否像应用"由你人工确认 3 个真实包。

**M1 交付记录（2026-11-03）**

落地内容（可对照 `src/`、`tools/`、`tests/`）：

- `src/core`：`byte_source`（文件/流两种数据源）、`zip_reader`（EOCD + ZIP64 + 中央目录 + raw deflate）、
  `string_pool`、`axml`（二进制清单）、`arsc`（含 sparse / Offset16 编码）、`apk_icon`（ID→密度挑选→启发式回退）、
  `wic_image`（WIC 解码 + PNG 编码）、`thumb_cache`（正/负缓存 + 200 MB 修剪）、`file_util`、`log`。
- `src/thumbs`：`IThumbnailProvider` + `IInitializeWithStream` / `IInitializeWithItem` / `IInitializeWithFile`，
  `DllGetClassObject` / `DllCanUnloadNow`，无注册导出（注册表由脚本写）。
- `tools/register.ps1`：`-Install` / `-Uninstall`，默认写 `HKCU`（免 UAC），带备份/回滚，卸载零残留。
- `tools/build.cmd`：一次完成 vcvars + CMake + Ninja（本机无 VS2022 generator，用 Ninja）。
- `tests/gen_fixtures.py`：用本机 `aapt2` 生成**真实** AXML/ARSC 的 APK（4 个正常 + 8 个坏包），
  并附 `MANIFEST.json` 期望值；`tests/thumbtest.cpp` 是验收工具（probe / 单文件 / 批量 / `--shell` / 清缓存）。

与计划的偏差（均已写进 ADR）：

1. **缩略图跑在 dllhost 隔离宿主、输入是 IStream** —— 见 [ADR-0005](adr/0005-isolated-thumbnail-host-and-stream-input.md)。
   这是 M1 最重要的发现：只实现 `IInitializeWithItem` 时 Explorer 管线直接失败（`0x8004B200`）。
2. **工具链用 MSVC 14.51（VS 18 Community）+ Ninja**，而非 ADR-0004 写的 v143 + VS 生成器：
   本机没有 VS2022 生成器，CMake 3.31 也不认 VS 18 生成器；`/MT`、`/W4 /WX`、`/guard:cf` 等约束不变。
3. **TD-001 已在 M2 关闭**（2026-11-04）：改为对 Stored 的 arsc 流式跳读；19.7 MB 真表 0.7–1.5 ms。
   遗留项是 deflate 的大 arsc（TD-002）。
4. adaptive icon 在本阶段用"同名光栅回退"，不做图层合成（原计划 M2 合成）。

验收数字见 [verification.md §10](verification.md)：冷 P50 8.2 ms / P95 14.9 ms（100 个包），
热 P50 1.3 ms；真实 Explorer 管线冷 12–21 ms（首次含 dllhost 启动 100–160 ms）。

**M2 实施记录（2026-11-04）**

- `src/core/arsc_stream.{h,cpp}`：流式 arsc。表头 → 全局字符串池头 → 包列表只读头部；
  查资源时只读目标 type chunk 的**头**（≤256 B）+ 单个条目偏移 + 条目值；取字符串时只读
  池偏移表一项 + 该字符串。deflate 的 arsc 无法随机寻址 → 回退有界整读（TD-002）。
- `src/core/byte_source`：新增 `SliceSource`（父源上的只读窗口），`ZipReader::OpenStoredEntry`
  给 Stored 条目零拷贝视图；`srccore` 所有权从 `unique_ptr` 改为 `shared_ptr`。
- `src/core/apk_info`：语言/密度改为遍历 type chunk 头（不再依赖整表），label 解析走流式。
- 夹具：`tests/gen_big_arsc.py` 生成 19.7 MB 真 arsc（4000 长字符串 × 3 语言 → Stored），
  外加 `tdeflate_arsc.apk`（同一张表以 Deflate 重打包，用来验证回退路径）。
- **顺带修掉的隐蔽 bug**：流式包头只读了 256 字节却访问偏移 284 的 `typeIdOffset`，
  越界垃圾让该包的 type 全部对不上号 —— 表现是"图标静默消失、只走启发式"。
  在 `qwxds-1.6.1.apk`（混淆包）上暴露，修复后 11/11 包与 aapt2 一致。

**M2 实施记录 · 签名与 features（2026-11-04）**

- `src/core/apk_signature.{h,cpp}`：v2/v3/v3.1 走 APK Signing Block（只读中央目录前的几 KB），
  v1 走 `META-INF/*.(RSA|DSA|EC)` 的 PKCS#7；证书摘要 = DER 的 SHA-256。
  不做摘要校验，`scheme` 语义是"检测到的签名块"（`base2_1.92G` 是活例子：v1 块在，验证器跳过）。
- `src/core/apk_info`：新增 `features`（`uses-feature`，`android:required` 缺省 true）。
- `ZipReader` 增加 `cd_offset()/cd_size()/ReadRange()`；`tests/thumbtest.cpp` 的 `--file/--dir/--list`
  统一转绝对路径（相对路径在 `--shell` 模式下必失败，`0x80070002`）。
- 夹具：`tests/gen_signed_fixtures.py` 造 v1-only（targetSdk 28 老包，否则 apksigner 自己拒收 v1-only）/
  v2-only / v3-only；对照脚本 `tests/verify_signature.py`（22/22）与 `verify_apkinfo.py`（features，11/11）。
- 代价：每个 worker 进程多 3–7 ms（crypt32/ASN.1 首次初始化），缩略图路径不受影响。

**M2 完成度（2026-11-04 收尾，下一会话从这里继续）**

| 交付项 | 状态 |
| --- | --- |
| `apkinfo.exe --json --schema=1`（file/app/icon/components/permissions/diagnostics） | ✅ |
| ZIP64 / AXML / ARSC（含 sparse、Offset16、语言/密度汇总） | ✅ |
| ARSC 分段流式（TD-001） | ✅ 19.7 MB 真表 0.7–1.5 ms |
| `--hash=sha256`（与 certutil 一致）、退出码 0/2/3、8 s watchdog | ✅ |
| 与 `aapt2 dump badging` 交叉校验 | ✅ 11/11 |
| `signature`（v1 / v2 / v3 方案 + 证书摘要） | ✅ 22/22 与 apksigner 一致（R-0005） |
| `features`（uses-feature） | ✅ 11/11 与 aapt2 一致 |
| `splits`（.apks 分包） | ✅ 随 M4a 落地（2026-11-04）：`container.splits`，见 [architecture.md §2.3](architecture.md) |
| TD-002：deflate 的大 arsc | ⬜ |

**签名块（已落地，2026-11-04；细节与实测见 [verification.md R-0005](verification.md)）**

实现要点（当初的清单，逐条对应到现在的代码）：

1. `ZipReader::cd_offset()/cd_size()/ReadRange()` —— 签名块贴在中央目录前面，必须偏移读。
2. 块 = `[uint64 size][pairs][uint64 size]["APK Sig Block 42"(16 B)]`，size 不含第一个 size 字段，
   所以块起点 = `cd_offset - (size + 8)`、pairs 区结束于 `size - 16`。（当初写的是 `cd_offset - size`，实测差 8。）
3. id：v2 `0x7109871a`、v3 `0xf05368c0`、v3.1 `0x1b93ad61`（真实包反查确认）。
4. value 是**三层**长度前缀：`lp(signers 序列)` → 每个 `lp(signer)` → `lp(signed data) lp(signatures) lp(pk)`；
   signed data = `lp(digests) lp(certs) lp(attrs)`（v3/v3.1 的 min/max SDK 在 certs 之后）。
   证书 DER 的 SHA-256 就是属性页"签名摘要"那一行的值。
5. v1：`META-INF/*.(RSA|DSA|EC)` 走 `CryptQueryObject`（PKCS7_SIGNED），每块取第一条证书。
   全都没找到 → `signature` 字段整体省略。
6. 验收：v1-only / v2-only / v3-only 三个自造夹具 + 真实包共 22 个样本，与 `apksigner verify --verbose
   --print-certs` 的方案集合与证书 SHA-256 全一致（`tests/verify_signature.py`）。

**M2 剩余（下一步）**

1. `splits`（`.apks/.xapk/.apkm` 的分包列表）→ 随 M4a 落地；`.apk` 单包不产生该字段。
2. TD-002：deflate 的大 arsc（> 8 MB）目前跳过，需要可重定位的流式解压。
3. V8 恶意样本回归、长跑（句柄 / 负缓存过期）。
4. `permissions[].level`（保护级别）需要框架侧 `framework-res` 数据，v1 先不提供（只给 `declared`）。

**风险 / 已批准的技术债**：ARSC 分段读的实现复杂度高于预期 → M1 先"整读 `resources.arsc`（≤ 8 MB；超限回落文件名启发式）"，登记为 **TD-001**（见 §5），M2 必须关闭。该债已于 2026-11-03 获批准。

## M2 · 解析核心 + worker

**范围**：把"全量元数据"从 DLL 里彻底搬出去，做成可独立测试的 CLI。

**交付**

1. `src/core`：ZIP64、AXML 完整解析、ARSC 完整（含 adaptive icon 合成所需引用）、签名块（v1/v2/v3 方案识别 + 证书摘要）、ABI/密度/locale/权限/组件计数。
2. `apkinfo.exe --json --schema=1`，输出符合 [architecture.md](architecture.md) §2.3 的 schema（冻结 v1）。
3. 测试：`tests/` 用 Go 测试或 Python 脚本跑 fixtures，断言关键字段；对真实包与 `aapt2 dump badging` / `apkanalyzer` 对照，差异单独列表。
4. 安全限额落地（[formats.md](formats.md) §8），用 3 个构造样本（条目炸弹 / 超大 arsc / 路径穿越）验证拒绝行为。

**验收**

- 10 个真实 APK：包名/版本/SDK/ABI/权限条目与 `aapt2` 一致（允许"我们多显示/顺序不同"）；
- 恶意样本：拒绝且退出码 = 2，不产生临时文件；
- 单包解析 P95 ≤ 300 ms（≤ 50 MB 包）、≤ 1.5 s（≤ 500 MB 包）；
- `--hash=sha256` 与 `certutil` 结果一致。

**退出条件**：schema v1 冻结（之后只增字段）；worker 可被属性页与脚本两类调用方稳定消费；**TD-001 关闭**——ARSC 改为分段流式读取，"整读 ≤ 8 MB"路径删除，且大资源表样本（≥ 32 MB）的缩略图仍满足 V2 判据。

## M3 · 属性页

**设计方案**：[m3-property-page.md](m3-property-page.md)（2026-11-04 评审通过）。
**实施记录**：见该文档 §14 与 [verification.md R-0006](verification.md)。

**范围**：`apkprop.dll`，只做「安装包信息」单页。

**交付**

1. `IShellPropSheetExt` + `IPersistFile`；`AddPages` ≤ 50 ms。
2. 页面布局：顶部（图标 48×48、应用名、包名、版本）+ 4 个分组（应用 / 技术 / 权限与组件 / 文件与签名）。
3. 异步：`PostMessage` 起 worker → 管道读取 → `WM_APP+1` 回填；解析中显示骨架与"正在解析…"。
4. 错误态：非安装包 / 损坏 / 超时 / worker 崩溃，各有明确文案 + "复制诊断"。
5. 可复制字段；"复制全部为 JSON"。
6. 亮暗跟随宿主 + 高 DPI（`WM_DPICHANGED`）。

**验收**

- 冷开 P95 ≤ 1 s（500 MB 包 ≤ 3 s），热开（缓存）≤ 200 ms；
- 解析进行中直接关窗 × 20 次：不崩、不残留进程、句柄数回到基线；
- 连续开关 100 次：explorer 句柄/私有提交无单调增长；
- 高 DPI 150%/200% 下不裁切；页内配色与属性窗口一致；
- **worker 人为 abort**：页面显示"解析器异常退出"，explorer 存活。

**退出条件**：属性页在 3 个真实包 + 2 个损坏包上表现符合预期；冻结页面字段顺序（之后只加不删）。

**M3 完成度（2026-11-04）**

| 交付项 | 状态 |
| --- | --- |
| `IShellPropSheetExt` + `IPersistFile` + **`IShellExtInit`**（后者是这个 build 的硬要求） | ✅ |
| 单页（5 组字段、分组带可折叠默认收起、复制、快捷键；原生 ListView 实现） | ✅ |
| 异步：`AddPages` 零 IO → 线程 → Job 里的 `apkinfo.exe` → `WM_APP` 回填 | ✅ |
| 错误态（缺 worker / 退出码 2 / 超时 / 崩溃 / 输出异常 / 不支持的格式） | ✅ 页内文案 + 重试 |
| 按需 SHA-256（按钮） | ✅ 172 ms（7.9 MB）/ ~1.2 s（1.9 GB） |
| 注册脚本 `-Feature Propsheet`（HKCU，免 UAC） | ✅ |
| 亮暗跟随系统（页内不自带配色） | ✅ 2026-11-04 改为原生控件：配色/主题全部由系统与宿主决定 |
| 150%/200% DPI、100 次开关句柄、卸载 diff | ✅ DPI 由布局纯函数 20,832 组合断言覆盖；100 次开关实测句柄无增长（[R-0011](verification.md)）；真实缩放渲染未做人眼确认 |
| 中英双语 | ⬜ M5 |

## M4 · 全格式

**M4a 实施记录（2026-11-04，记录 [R-0011](verification.md)）**

- `src/core/package.{h,cpp}`：容器侦查 + base 选择 + base 数据源。单 APK 也走这条（根上有
  `AndroidManifest.xml` 就是 APK），所以缩略图与 worker 只有一条代码路径。
- `src/core/byte_source.cpp` 新增 `MemorySource`（deflate 的 base 只能整块解压）；Stored 的 base 走
  `SliceSource`，**PUBG 的 1.25 GB xapk 只读 107 MB 的 base，不整包进内存**。
- `apk_info` 拆成 `ScanApkZip(zip, ...)` + `CollectApkInfoFromSource`（先判容器再解析），JSON 增加
  `container` 块（见 [architecture.md §2.3](architecture.md)），`kind` 现在会写 `xapk/apks/apkm/splits`。
- 缩略图：`thumb_provider` 改用 `ReadPackageIcon()`（定位图标 → 读字节 → 解码），容器根 `icon.png` 是兜底。
- 属性页：`page_model` 增加「安装包类型」「内层 APK」两行与可折叠「分包 · N 项」组。
- `register.ps1` 默认扩展名改为 `apk, xapk, apks, apkm`（`-Extensions 'apk'` 可退回只注册 APK）。
- 新增 `tests/verify_container.py`（与 aapt2 + ZIP 交叉校验）与 `tests/gen_container_fixtures.py`
  （4 个坏容器：无 APK / 坏 base / 假炸弹 / 截断）。
- **只测过 `.xapk` 真实样本**；`.apks`（bundletool）与 `.apkm`（APKMirror）只有启发式 + 坏样本，
  拿到真实样本前不要宣称支持。

**M4d 实施记录（2026-11-04，记录 [R-0012](verification.md)）**

- `src/core/hap.{h,cpp}`：`module.json` 解析（json_lite）+ `$media:` 图标三级启发式
  （名字 → layered json 的键名 → `startWindowIcon`/约定名/最大 PNG）。
- 判定顺序改成 HAP 优先：根上有 `module.json` 且没有 `AndroidManifest.xml` → HAP；否则走 APK / 容器。
  三条路径（缩略图、worker、属性页）仍然共用同一份判定与同一个 `ZipReader`。
- JSON 增加 `hap` 块；**不把鸿蒙 API 号塞进 `app.minSdk/targetSdk`**；属性页技术组显示
  模块 / 主 Ability / API 版本 / 编译 SDK / 设备类型 / Ability 计数，文件组显示「安装包类型 HAP（鸿蒙）」
  与「名称引用 `$string:app_name`（v1 不解析）」。
- `register.ps1` 默认扩展名加 `.hap`。新增 `tests/verify_hap.py`（对 `module.json` + 图标字节/尺寸交叉校验）。
- **v1 明确不做**：`$string:` 标签解析（要解 `resources.index`，RestoolV2 私有格式）、layered icon 的合成
  （只取 foreground 层）、`.hsp`/`.app` 多模块套件。拿到更多样本前不要扩大承诺。

按"复用成本从低到高"排序，每个子阶段独立验收：

| 子阶段 | 内容 | 估时 | 关键验收 |
| --- | --- | --- | --- |
| M4a | `.apks/.xapk/.apkm`（外层 ZIP + base.apk；`icon.png` 兜底） | 1 d | ✅ 2026-11-04（[R-0011](verification.md)）：2 个真实 xapk 与 aapt2 一致、Explorer 里出图标、属性页有「分包」组 |
| M4b | `.aab`（`BundleConfig.pb`、**protobuf manifest**、`resources.pb`） | 2–3 d | 3 个 AAB：包名/版本/模块列表正确；图标与 bundletool 生成的一致 |
| M4c | `.ipa`（binary plist；AppIcon 约定名 → iTunesArtwork → `Assets.car` 回退） | 2 d | 3 个 IPA：bundleId/版本正确；≥ 2 个能出高清图标（car 失败要能回退） |
| M4d | `.hap`（`module.json`；`$media:` → `resources.index` 启发式 → 索引解析） | 2–3 d | ✅ 2026-11-04（[R-0012](verification.md)）：2 个真实 HAP 与 `module.json` 一致、Explorer 出图标、属性页有模块/API/权限组；`$string:` 标签与 layered 合成未做 |
| M4e | `.appx/.msix`（`AppxManifest.xml` + 限定符选图） | 0.5 d | 2 个包正确 |

**验收（跨格式）**

- 一个混合目录（每种格式 ≥ 5 个）滚大图标视图：不崩、无卡顿尖峰（ETW 里单次 `GetThumbnail` ≤ 300 ms）；
- 每种格式的属性页字段与对应官方工具（bundletool / aapt2 / plutil / hvigor）抽查一致；
- 格式识别不依赖扩展名（嗅探容器），扩展名只用于注册表路由。

**退出条件**：六类格式的"图标 + 属性页"都能工作，负缓存与错误文案覆盖到新格式。

## M5 · 交付打磨

**交付**

1. 安装器（一次 UAC 完成注册；卸载精确回滚）与便携版（`register.ps1`）。
2. VERSIONINFO、**格式差异化 ico（每扩展名一枚，16/24/32/48/256 五档）与 `DefaultIcon` 注册**、可选代码签名（OV/EV 待定，见 design.md §8）。
3. 诊断开关（`%TEMP%\apk-shell-ext.debug.on` 时写日志，参照果核的做法）。
4. 中英文资源；高 DPI 全面回归。
5. 缓存清理工具（`tools\clearcache.ps1` 或 `apkinfo.exe --clear-cache`）。
6. 评估 `IExtractIcon`（小图标视图）与 MSIX：只做结论，不默认实现（需改 ADR-0001）。
7. 更新全部文档与验收记录。

**验收**

- 真装真卸（一次 UAC）：装后 3 个格式生效；卸后注册表/文件零残留；
- ProcMon 回归 V1（仍是 0 次打开）；
- 小图标 / 列表视图各格式图标可区分，且 V1 复跑仍为 0 次 `CreateFile`（V9）；
- Defender 全盘扫描无误报；SmartScreen 未签名时提示可接受（记录截图）；
- 卸载重装后缩略图仍正常（缓存键含版本号）。

**退出条件**：可交付给自己日常使用，且文档与实现一致（"文档说的 = 代码做的"）。

## 5. 技术债登记

> 技术债必须有编号、关闭条件与关闭阶段；未关闭的债不得进入下一里程碑的"完成"定义。

| 编号 | 内容 | 引入阶段 | 影响 | 关闭条件 | 状态 |
| --- | --- | --- | --- | --- | --- |
| TD-001 | 缩略图路径**整读** `resources.arsc`（上限 8 MB，超限回落"按文件名猜 mipmap"） | M1 | 大资源表的包首次出图变慢（仍受 300 ms 超时保护）；内存瞬时多占 ≤ 8 MB | M2 改分段流式读取并删除整读路径；≥ 32 MB 资源表样本仍满足 V2 | ✅ **已关闭（2026-11-04）**：Stored 的 arsc 改为在文件上开窗口、按 chunk 跳读（`src/core/arsc_stream.cpp`）；19.7 MB 真表 0.7–1.5 ms；细节见 [verification.md R-0004](verification.md) |
| TD-002 | **deflate** 的 `resources.arsc` 且 > 8 MB 时仍会跳过（deflate 流不可随机寻址） | M2 | 这类包的名称/语言/密度可能缺失，图标回落启发式 | 实现可重定位的流式解压（或先解压到临时文件并由限额保护） | 新增（2026-11-04），未关闭。实测真实包几乎都是 Stored |

## 6. 风险登记

| # | 风险 | 影响 | 概率 | 应对 |
| --- | --- | --- | --- | --- |
| R1 | AAB 的 protobuf manifest 工作量超预期 | M4 延期 | 高 | 先只支持 `base` 模块；protobuf 用 lite 运行时或手写 decoder；必要时 M4b 单独发布 |
| R2 | `Assets.car` 格式无公开文档 | IPA 图标退化 | 中 | 首选约定名/iTunesArtwork 回退；car 只做"最大尺寸 PNG 提取" |
| R3 | 鸿蒙 `resources.index` 无稳定公开结构 | HAP 图标靠猜 | 中 | `$media:` 名 → `resources/base/media/<name>.png` 启发式先覆盖；索引版留 M4d-2 |
| R4 | Win11 后续版本收紧 in-proc 扩展 | 全部失效 | 低 | 保持 Microsoft 官方接口、不注入、不 hook；关注 Shell 变更公告 |
| R5 | 杀软把 shell 扩展判为可疑 | 用户装不上 | 中 | 签名、VERSIONINFO、不写自启/不联网；提供 SHA-256 与源码 |
| R6 | ARSC 大包读取拖慢缩略图 | 违背 B1 | 中 | 允许 M1 技术债（整读 + 上限），M2 改分段；超限走"文件名启发式" |
| R7 | explorer 内 DLL 崩溃 | 桌面崩 | 低 | 全接口 `try/catch`；解析在 worker；M1 起就做 20 次异常样本回归 |
| R8 | 32 位宿主（Total Commander 等）需要 x86 版 | 覆盖不全 | 低 | M5 评估；x86 只是编译配置差异，但需单独测试 |

## 7. 每阶段结束必做（DoD）

1. 在 `docs/verification.md` 追加一条记录：命令、原始输出、结论、未验证项。
2. 更新本文件顶部的状态表与 `README.md` 的阶段行。
3. 有决策变更 → 新 ADR（编号递增，不修改旧 ADR 的结论，只追加修订段）。
4. 代码与文档不一致时，以"能跑的证据"为准，改文档。
