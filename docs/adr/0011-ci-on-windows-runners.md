# ADR-0011 · CI 形态：全原生 Windows runner，tag 驱动发版

- 状态：**已采纳**
- 日期：2026-11-08
- 相关：[release.md](../release.md)、[development.md §2](../development.md)、[ADR-0010](0010-arch-specific-builds-and-installers.md)

## 上下文

要把"多架构编译 → 打包 → 发版"交给 GitHub Actions。可选的形态有三类：

| 形态 | 编译 | 打包 | 问题 |
| --- | --- | --- | --- |
| 全原生 Windows runner | MSVC（`windows-latest` / `windows-11-arm`） | Inno（Windows） | 无 —— 与本地开发链完全一致 |
| Windows 编译 + Linux 打包 | MSVC | Inno/WiX under Wine | 多一条 Wine 链路；WiX 官方不支持 Linux 原生构建，社区可靠做法也是 Wine |
| 全 Linux runner | clang-cl+xwin 或 MSVC under Wine | Wine | 要推翻 ADR-0004 的 MSVC 边界，`/WX`、`/guard:cf`、`.rc`、`.def` 全部重验 |

事实基线：

- 仓库是 **public**，`windows-latest` 与 `windows-11-arm`（Windows 11 Desktop Arm64 镜像）都免费；
  `windows-11-arm` 只在 public 仓可用（private 仓会直接失败）。`[上游]`
- 本项目的验收是**二进制/行为级**的（ProcMon 零读取、PE 机器码、真实 Explorer 管线、dllhost 隔离），
  CI 里没有资源管理器，也没有 Arm64 真机的桌面会话。
- 安装包依赖 Inno 的 Pascal `[Code]`（改名让路、撞车探测、功能开关）共 ~250 行；
  换成 WiX/NSIS 等于重写并重新验收一遍安装期行为。

## 决策

1. **CI 只用原生 Windows runner**，不引入 Wine / Linux 打包链：
   - `x64` 腿：`windows-latest`（原生 MSVC x64）；
   - `arm64` 腿：`windows-11-arm`（原生 Arm64，`tools\build.cmd Release arm64` 在该机上就是本机编译）。
2. **打包在 x64 runner 上做两份**（`pack.cmd x64` / `pack.cmd arm64`），
   因为 arm64 包的内容就是 `out\arm64` 的产物 + 同一份 `.iss`；不需要 Arm64 机器去跑 ISCC。
3. **触发方式**：`workflow_dispatch`（手动）与 `push: tags: v*`（发版）。
   **不做 PR 门禁、不做分支保护必需检查**——按项目既有偏好，非构建必需的工具体验不设闸门。
4. **发版**：tag 触发时由 CI 调 `gh release create`，附两个安装包与 `SHA256SUMS.txt`；
   版本号仍以 `src/version.h` 为唯一来源，CI 里跑 `sync-version.ps1 -Check` 作为漂移闸门
   （漂移就红，不自动改文件）。
5. **act 只用于本地自检 workflow 本身**（YAML 语义、表达式、job 图、`-self-hosted` 下真跑一遍 Windows 腿），
   不把 act 当验收手段。
6. **CI 绿 ≠ 验收绿**：verification.md 里那些判据（零 CreateFile、300 ms 预算、装/卸零残留、Arm64 运行期行为）
   仍需人工在真机上执行；CI 只保证"两个架构都能编出正确机器码的二进制、都能出包、版本号不漂"。

## 后果

**正面**

- 构建链与本地完全一致（同样的 `build.cmd` / `pack.cmd`），CI 里踩到的坑就是本地会踩的坑。
- Arm64 产物由**原生 Arm64 runner** 编译，顺带给出"Arm64 上运行期是否可用"的验证入口
  （runner 上可以跑 `propsheet_model_test --selftest` 这类非 GUI 测试）。
- 不需要处理 Wine 版本、`.NET` 运行时、MSI 语义这些额外变量。

**负面 / 需要接受**

- `windows-11-arm` 是 public preview：排队可能变长、镜像内容可能变动，且 private 仓不可用。
  一旦它不可用，Arm64 腿退回"本地交叉编译 + 人工上传"，构建机制不变。
- 全量夹具（`tests/fixtures/generated`，依赖 aapt2 生成）不入库，CI 里跑的是入库的 5 个最小样本
  （见文末修订段），真实包/坏包/大包的回归仍需人工在本地跑。
- 无代码签名：安装包与二进制仍是未签名状态，SmartScreen 提示不变。

## 替代方案

| 方案 | 否决原因 |
| --- | --- |
| Linux runner + Wine 打包 | 编译端仍是 MSVC/Windows；省下的是免费的 Windows 分钟，多的是失败链 |
| WiX v4+ 直接跑 Linux 原生 | 官方不支持（依赖 Windows 组件），社区做法仍是 Wine；且要把 `.iss` 的 `[Code]` 重写成自定义动作 |
| 全 Linux + clang-cl/xwin | 推翻 ADR-0004 的 MSVC 边界，`/WX` 清零与 `/guard:cf`、WTL、`.rc`、`.def` 全要重验 |
| 引入 PR 门禁 | 与"非构建必需工具不设闸门"的项目偏好冲突；构建本来就在本地跑 |

## 修订条件

- 若 CI 时长/配额成为问题，或需要私有仓镜像，再评估自建 runner（Windows x64 + Arm64 各一台）。
- 若开始做代码签名，签名步骤进 `package` job（证书走 secrets），不改变 job 拓扑。
- 若夹具方案定了（入库 or 装 SDK），把 `thumbtest` 全量样本加进 `build` job，并在 verification.md 记一条 R 号。

## 修订段

### 2026-11-08 · 夹具方案定为「入库小样本」

两条候选里选了入库：`tests/fixtures/ci/` 放 5 个逐字节副本（PNG / WebP / 自适应 / deflate 的 arsc / 无图标，
合计 ~16 KB），来源是 `tests/fixtures/generated/`（`tests/gen_fixtures.py` + aapt2 36.0.0 产出）。
理由：runner 上装 Android SDK 只为跑 5 个包不划算，而 aapt2 仍是"造样本"的必要工具、不是"跑样本"的必要工具。

`build` job 因此多一步 `Thumbnail smoke`，断言写死为
`count == 5 && failed == 1（f_noicon，预期）&& coldP95 ≤ 300 ms`（300 是 architecture.md §2.1 的软上限）。
本机实测冷缓存 `count=5 failed=1 coldP95=126 ms`（其中 ~126 ms 是 WebP 编解码器首次初始化）。
改这个目录等于改 CI 基线，`tests/fixtures/ci/README.md` 里写明了同步规则。
