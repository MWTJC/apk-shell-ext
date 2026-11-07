# 架构决策记录（ADR）

> 只记录**会限制后续实现自由度**的决定：接口选型、进程边界、依赖边界、禁用项。
> 普通的实现细节不进这里（放 [architecture.md](../architecture.md)）。

| 编号 | 决策 | 状态 | 影响阶段 |
| --- | --- | --- | --- |
| [0001](0001-thumbnail-provider-only.md) | 缩略图只挂 `IThumbnailProvider`，v1 不挂 `IExtractIcon` | 已采纳（M5 已评估：不修订，见 [m5-delivery.md §6](../m5-delivery.md)） | M1 |
| [0002](0002-lazy-metadata-and-banned-handlers.md) | 元数据只走属性页；**禁止** `IPropertyStore` / 列 / 预览 / Infotip | 已采纳（禁止项） | M3 |
| [0003](0003-out-of-proc-parser.md) | 全量解析放独立 worker 进程，DLL 只做 UI/IPC | 已采纳 | M2/M3 |
| [0004](0004-stack-and-dependency-boundary.md) | C++20 / MSVC / CMake / 静态 CRT / WTL；禁止 MFC、Qt、.NET、自有解码器 | 已采纳（禁止项） | M1 |
| [0005](0005-isolated-thumbnail-host-and-stream-input.md) | 缩略图在 dllhost 隔离宿主里跑、输入是 IStream；解析核心改为 ByteSource，流模式缓存键含中央目录摘要 | 已采纳（M1 实测得出） | M1 |
| [0006](0006-format-scope-cut.md) | 格式范围收敛：只支持 `.apk/.xapk/.apks/.apkm/.hap`，**不做** `.aab/.ipa/.appx/.msix` | 已采纳（2026-11-04） | M4 |
| [0007](0007-inno-setup-per-user-install.md) | 安装/分发用 Inno Setup 6；默认按用户装（零 UAC），`/ALLUSERS` 才一次 UAC；注册表根用 `HKA`；装好之后的命令并入 `apkinfo.exe` | 已采纳（2026-11-04） | M5 |
| [0008](0008-feature-toggles-and-upgrade-policy.md) | 缩略图/静态图标各有开关（安装期 `[Tasks]` + 运行期 `--features`），属性页恒开；安装期探测别人的 handler 并询问，默认保持原样、不写不撤；升级就地追加卸载日志，不拦降级 | 已采纳（2026-11-04；§4 第 2 条"升级关应用"被 [0009](0009-rename-in-place-upgrade.md) 取代） | M5 之后 |
| [0009](0009-rename-in-place-upgrade.md) | 升级**不关闭** Explorer / dllhost：把被加载的二进制改名成 `*.old` 让位，再写新文件；生效时间推迟到资源管理器下次启动 | 已采纳（2026-11-07，1.0.2） | M5 之后 |

## 写作规则

- 文件名：`NNNN-kebab-title.md`，编号连续不复用；标题用中文即可。
- 结构固定：**状态 / 日期 / 上下文 / 决策 / 后果（正面·负面）/ 替代方案与被否原因 / 修订条件**。
- 已被推翻的 ADR 不改结论，状态改为「已被 ADR-XXXX 取代」，并在末尾追加修订段说明原因与迁移方式。
- 每条决策必须能回答："如果不这么做，会违反 design.md 的哪一条（B1/B2/B3 或 P1–P5）"。
