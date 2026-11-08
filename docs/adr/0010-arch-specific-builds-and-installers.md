# ADR-0010 · 双架构构建（x64 / arm64）与按架构分包

- 状态：**已采纳**
- 日期：2026-11-08
- 相关：[architecture.md §4/§5](../architecture.md)、[development.md §2/§3](../development.md)、[ADR-0004](0004-stack-and-dependency-boundary.md)、[ADR-0007](0007-inno-setup-per-user-install.md)

## 上下文

Windows on Arm 上，资源管理器与它拉起的 COM 宿主（`dllhost.exe`）都是**原生 Arm64 进程**。
按 Microsoft 的架构互操作规则（"an Arm64 process can only load Arm64 binaries"），x64 的进程内 COM
组件在 Arm64 上**无法被加载**——写注册表、装文件都成功，功能静默不生效。`[上游]`

本项目三个产物里有两个是进程内 COM（`apkthumb.dll` / `apkprop.dll`），因此 Arm64 支持不是"性能优化"，
而是"能不能用"的问题。实测补充：

| 事实 | 证据 |
| --- | --- |
| 现有产物是纯 x64 | `out/x64` 六个 PE 头 `machine=0x8664` `[实测]` |
| 源码不需要任何架构分支即可编出 Arm64 | 交叉编译 47/47 步全过（`vcvarsall x64_arm64` + 原 CMake，`/W4 /WX`、`/guard:cf`、`/MT` 全部保持），六个产物 `machine=0xaa64` `[实测]` |
| Arm64 产物只依赖系统 DLL | 导入表：`KERNEL32 GDI32 SHLWAPI ole32 bcrypt`（thumb）+ `SHELL32 USER32 COMCTL32 ADVAPI32`（prop）+ `CRYPT32`（exe）；无 VC 运行时、无第三方运行时 `[实测]`，ADR-0004 的依赖边界在 Arm64 上同样成立 |
| **Arm64EC 不能解决这个问题** | Arm64EC 只与 x64/Arm64EC 进程互通，不能被原生 Arm64 进程加载（官方互操作表）`[上游]` |

## 决策

1. **只做两种产物架构：`x64` 与 `arm64`（PE `AA64`）**。不做 Arm64EC、不做 Arm64X：
   - Arm64EC 被原生 Arm64 的 explorer/dllhost 拒绝加载，对本场景无效；
   - Arm64X（一份二进制同时喂 x64 与 Arm64 进程）技术上成立且是 Microsoft 对 plugin 的建议形态，
     但它要求同一映像内两套代码、构建链更复杂，对 x64 用户没有收益，留作后续选项（见"修订条件"）。
2. **构建**：CMake 增加缓存变量 `APKSX_ARCH`（`x64` 默认 / `arm64`），产物落 `out\<arch>`；
   `tools\build.cmd [Release|Debug] [x64|arm64] [--clean]`，交叉编译走 `vcvarsall x64_arm64`，
   Arm64 使用独立构建目录 `build-arm64`。**源码中不出现任何 `#ifdef _M_ARM64` 之类的架构分支**。
3. **分发按架构分包**：一个架构一个安装器，靠**文件名**区分
   （`apk-shell-ext-setup-<ver>-x64.exe` / `apk-shell-ext-setup-<ver>-arm64.exe`）。
   同一个 `.iss` 由 ISCC `/DBUILD_ARM64` 编两次，不维护两份脚本。
4. **安装器架构守卫**：x64 包 `ArchitecturesAllowed=x64os`（**不是** `x64compatible`），
   arm64 包 `ArchitecturesAllowed=arm64`；`ArchitecturesInstallIn64BitMode` 跟随同一标识。
   理由：`x64compatible` 会把包装进 Arm64 机器，而装上去的 DLL 不会被任何宿主加载
   （属性页不出现、缩略图不出图），"静默失效"比"拒绝安装并说明原因"糟得多。
5. **其余契约不变**：CLSID、注册表布局、缓存键、功能开关、升级改名让路（[ADR-0009](0009-rename-in-place-upgrade.md)）
   全部沿用；同一台机器上只会有一套（Arm64 与 x64 共用同一个 64 位注册表视图，不能同时注册两个架构的
   `InprocServer32`）。

## 后果

**正面**

- 一份源码、一条构建入口，两种产物；Arm64 上属性页与缩略图才可能真正工作（运行期仍需实机验收）。
- 错装被安装器拦住，不会出现"装成功但什么都不生效"的支持工单。
- x64 版行为零变化（`x64os` 在 x64 机器上的匹配结果与原来的 `x64compatible` 相同）。

**负面 / 需要接受**

- 发布清单从 1 个文件变 2 个，`README` / `install.md` / `release.md` 的"下载哪个"要写清楚。
- `out/x64` 这个旧路径在脚本与文档里到处都有，本次一并改成 `out\<arch>`；老会话的接力说明会短暂过期。
- **Arm64 的运行期验收还没有做**：WebP 的 WIC 编解码器在 Arm64 上是否可用、`dllhost` 隔离管线是否照旧
  喂 `IStream`、300 ms 预算是否仍成立，都必须在真机/VM 上量（见 [verification.md](../verification.md) 待补条目）。
- 两套安装器意味着两条手工 smoke 路径；在 CI 接管之前，Arm64 那条最容易漏测。

## 替代方案

| 方案 | 否决原因 |
| --- | --- |
| 双架构单包（`[Files]` 里 `Check: IsArm64` 二选一） | 用户明确要求按架构分包；且 `Check` 分支会让文件/注册表条目翻倍，验证面比两个小包更大 |
| Arm64EC 重编 | 不能被原生 Arm64 进程加载（见上文 `[上游]`），对本场景等于没做 |
| Arm64X 单二进制 | 构建复杂度高（同映像两套代码）、体积与 `/WX` 判定要重测；收益仅"少一个文件" |
| 在 Linux runner 上交叉编译打包（WiX / Inno under Wine） | 编译端仍必须是 MSVC/Windows（[ADR-0004](0004-stack-and-dependency-boundary.md)），见 [ADR-0011](0011-ci-on-windows-runners.md) |

## 修订条件

- 若后续需要把 provider 当"插件"塞进别人的进程（第三方宿主、Arm64X 宿主），重新评估 Arm64X。
- 若 Microsoft 让 Arm64 的 shell 具备跨架构托管（目前只有右键菜单有这条 shim），可以退回到"只发 x64 包"，
  但需要新的实测证据，不接受"据说"。
- 若 `windows-11-arm` 托管 runner 长期不可用（仅 public 仓、public preview），Arm64 产物改为"手工本地交叉编译 + 人工上传"，
  构建机制不变。
