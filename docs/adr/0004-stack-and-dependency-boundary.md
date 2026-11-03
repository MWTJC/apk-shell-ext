# ADR-0004 · 技术栈与依赖边界

- 状态：**已采纳**
- 日期：2026-11-03
- 相关：[architecture.md §5](../architecture.md)、[design.md §P2](../design.md)

## 上下文

壳扩展的候选技术栈与它们在本场景的实测/已知问题：

| 栈 | 代表 | 问题 |
| --- | --- | --- |
| C++ + ATL/WTL | HashTab 5.1.0.23（739 KB，静态 CRT，Crypto++） `[实测]` | 无；需要自己写 COM 样板 |
| C++ + 自研 UI 框架 | 果核看图（`core-ui.dll` 4.8 MB + `ghde.dll` 6.9 MB） `[实测]` | 框架体积大，只在自家 app 里划算；壳扩展侧仍是最小 provider |
| .NET + SharpShell | ApkShellext2 | 依赖 .NET Framework 4.x；Win11 有失效报告（issue #120）；每次加载 CLR 有固定开销 |
| Rust + windows crate | — | 可行且内存安全，但 COM 样板与调试成本更高；团队熟悉度不足 |
| Electron/Qt | — | 进 explorer 进程完全不现实 |

## 决策

1. **语言/工具链**：C++20 + MSVC v143（VS2022 Build Tools）+ CMake ≥ 3.27。
2. **UI**：`apkprop.dll` 用 **WTL + 对话框模板**（与 HashTab 同路线，产物小、DPI 由系统处理）。
3. **CRT**：**静态链接 `/MT`**，DLL 侧禁止 `/MD`（避免 VC redist 缺失导致静默失效）。
4. **允许的导入**：`kernel32 user32 shell32 shlwapi ole32 oleaut32 gdi32 olegdi? windowscodecs advapi32 bcrypt crypt32 uxtheme dwmapi` —— 全部系统组件。
5. **禁止的依赖**：MFC、Qt、.NET/CLR、任何 > 1 MB 的第三方静态库、任何自有 PNG/WebP 解码器（解码只用 WIC）。
6. **安全基线**：`/guard:cf`、`/DYNAMICBASE`、`/NXCOMPAT`、`/W4 /WX`；COM 边界全部 `try/catch(...)` 兜底。

## 后果

**正面**

- 两个 DLL 合计可控制在 500 KB 内，explorer 内存增量小（V4 判据 ≤ 3 MB）。
- 不依赖任何运行库，装机即用；杀软误报面小（不联网、不注入、不写自启）。
- 解码走系统 WIC，自动继承 Windows 对 libwebp/PNG 的安全补丁（规避 CVE-2023-4863 这类重演）。

**负面**

- C++/COM 样板代码多（`IThumbnailProvider` + `IInitializeWithItem` + 类厂 + 注册），M1 需要先写骨架。
- WTL 无官方 NuGet 式分发，需 vendor 源码（与既有项目习惯一致）。
- 无垃圾回收，句柄/内存管理要自己严格（V7 有句柄泄漏用例）。

## 替代方案

| 方案 | 否决原因 |
| --- | --- |
| C# / .NET（SharpShell） | 需 Framework 4.8 运行时；Win11 兼容性已有负面案例；explorer 内 CLR 常驻 |
| Rust（windows-rs） | 安全收益真实，但 COM 注册/调试/与 WTL 对话框混用成本高；可作为 worker 的后续选项 |
| C++ + MFC | 静态链接 MFC 体积大、DPI 处理差；WTL 已足够 |
| 纯 Win32（不用 WTL） | 可行，但对话框/DPI 样板更多；WTL 是薄头文件库，vendor 成本低 |

## 修订段

### 2026-11-03 · 技术栈确认

评审确认：**维持 C++20 + MSVC v143 + CMake + 静态 CRT + WTL，不引入 .NET / Qt / MFC**。依据是"性能优先"：explorer 内无 CLR 启动开销、无 VC redist 依赖、两个 DLL 合计可 < 500 KB（果核的 thumb dll 364 KB、HashTab 740 KB 是同类量级的参照 `[实测]`）。

同时确认边界不变：AAB 需要的 `protobuf-lite` **只允许出现在 `src/core`（worker 侧）**，不得进入任何 DLL；其在 M4b 实施记录里需补体积与许可证说明。DLL 侧任何新增依赖须另开 ADR。

## 修订条件

- 若未来要复用解析器做跨平台工具，可把 `src/core` 抽成独立静态库（当前已是库结构，无需改决策）。
