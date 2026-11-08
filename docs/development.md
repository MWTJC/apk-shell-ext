# 开发与构建

> 面向改代码的人。访客/使用者请看 [../README.md](../README.md) 与 [install.md](install.md)。
> 设计出发点在 [design.md](design.md)，组件与契约在 [architecture.md](architecture.md)。

## 1. 目录

```
apk-shell-ext/
├─ docs/            设计、架构、格式矩阵、路线图、验收、ADR、安装说明、发布流程
├─ installer/       Inno Setup 6 脚本（apk-shell-ext.iss）
├─ icons/           三枚格式 ico（16/24/32/48/256，由 tools/gen_icons.py 生成）
├─ src/
│  ├─ core/         解析核心（ByteSource / zip 中央目录、AXML、ARSC、签名块、图标定位、
│  │                容器侦查、WIC、缓存、i18n）
│  ├─ thumbs/       IThumbnailProvider 壳扩展
│  ├─ propsheet/    IShellPropSheetExt 壳扩展（原生控件单页）
│  ├─ worker/       apkinfo.exe（JSON 输出 + --status/--diag/--clear-cache/--lang/--features）
│  ├─ version.h     版本号唯一来源（VERSIONINFO / 安装器都从它派生）
│  └─ third_party/  miniz（raw deflate，MIT，只编 C 源）
├─ tools/           build.cmd、pack.cmd、sync-version.ps1、register.ps1、verify-install.ps1、
│                   gen_icons.py（+ fonts/ 里的 Nunito Black）、icon_probe.ps1、验收/截图脚本
└─ tests/           gen_fixtures.py（aapt2 造真实包）、verify_*.py（交叉校验）、验收工具
```

## 2. 构建

```bat
tools\build.cmd Release        :: 产物在 out\x64\{apkthumb.dll, apkprop.dll, apkinfo.exe, ...}
tools\build.cmd Release arm64   :: 交叉编译出 Arm64 产物到 out\arm64（ADR-0010）
tools\build.cmd Debug --clean  :: 清掉 build\ 重来（arm64 清的是 build-arm64\）
```

- 架构选择走 CMake 的 `-DAPKSX_ARCH=x64|arm64`（默认 x64），构建目录也随之分开（`build` / `build-arm64`）。
- Arm64 交叉编译需要 VS 装 **"MSVC v143+ - C++ ARM64 build tools"**；脚本用 `vcvarsall x64_arm64`，
  缺组件时 vcvarsall 会直接失败并提示。**源码里没有架构分支**——同一份代码编两个架构。

- 依赖：Visual Studio 的 C++ 工具链（MSVC v143 或更新）、CMake ≥ 3.27、Ninja。
- **工具链是自动找的**：先看 `%APKSX_VCVARS%`，再用 `vswhere` 找任意一个带 C++ 工作负载的 Visual Studio，
  最后试 `Program Files` 下的 2022/2019 常见布局。都没有时脚本会打印怎么用 `APKSX_VCVARS` 指定。
- **坑**：`apkprop.dll` 被 explorer 占用时链接会 `LNK1104`——先
  `Stop-Process -Name explorer -Force` 再编，编完 explorer 会自启。

## 3. 打包与版本号

版本号只有一个来源：`src/version.h` 的 `APKSX_VERSION_MAJOR/MINOR/PATCH/BUILD`
（`APKSX_VERSION_STRING` / `APKSX_VERSION_FULL` 必须与它自洽，脚本会校验）。

```bat
tools\pack.cmd                 :: 先同步版本号到 .iss，再 ISCC → out\dist\apk-shell-ext-setup-<ver>-x64.exe
tools\pack.cmd arm64           :: 同上，Arm64 包 → out\dist\apk-shell-ext-setup-<ver>-arm64.exe
tools\pack.cmd arm64 --build   :: 先 build.cmd Release arm64 再打包
```

- `tools\sync-version.ps1` 把 `src/version.h` 的版本写进 `installer\apk-shell-ext.iss` 的 `#define AppVersion`；
  `-Check` 只校验不改（发布前跑一次，避免包版本和二进制版本漂移）。
- `pack.cmd` 里 ISCC 的查找顺序：`%ISCC%` → `where ISCC.exe` → `Program Files\(x86)\Inno Setup 6` → 本机旧路径。
- 改版本只改 `src/version.h` 一处：`.rc` 的 VERSIONINFO 在编译时取它，安装器由脚本取它，
  安装包文件名用 `{#AppVersion}`。

## 4. 免安装调试

```bat
 powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install
 powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Uninstall
```

写 `HKCU`、不需要 UAC，支持 `-Feature Thumb|Propsheet|Icons`、`-Extensions apk`、`-DllDir/-IconDir`、
`-Scope LocalMachine`（需管理员）。

## 5. 测试夹具与验收

真实样本（`tests\fixtures\real\`）与生成的夹具（`tests\fixtures\generated\`）都**不入库**，按需重造：

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
```

安装器验收（装前快照 → 装 → 断言 → 卸 → 断言 → 快照 diff 必须为空）：

```bat
powershell -File tools\verify-install.ps1 -Phase snapshot -Path a.txt
out\dist\apk-shell-ext-setup-<ver>-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
powershell -File tools\verify-install.ps1 -Phase check -State installed
"%LOCALAPPDATA%\Programs\apk-shell-ext\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
powershell -File tools\verify-install.ps1 -Phase check -State removed
powershell -File tools\verify-install.ps1 -Phase snapshot -Path b.txt
powershell -File tools\verify-install.ps1 -Phase diff -A a.txt -B b.txt
```

只装了部分功能时，用 `-Features prop`（或 `-Features thumb,prop`）断言"没装的确实不存在"。

## 6. 装好之后的诊断命令

```bat
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --status
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag on|off|status|clear
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --clear-cache | --cache-info
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --lang zh|en|auto
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features [thumb=on|off][,icon=on|off][,all=on|off]
```

日志：存在 `%TEMP%\apk-shell-ext.debug.on` 时写 `%LOCALAPPDATA%\apk-shell-ext\apkthumb.log`。

## 7. 当前状态

**M0–M5 全部完成**（M1 R-0002 · M2 R-0005 · M3 R-0006/0009/0010/0011 · M4 R-0011/R-0012/R-0013 ·
M5 R-0014），收口后还有两轮改动：**图标 v2（R-0015）**、**功能开关与升级策略（R-0016）**，
以及**控制面板条目修正（R-0017）**。格式范围见 [ADR-0006](adr/0006-format-scope-cut.md)，
分发见 [ADR-0007](adr/0007-inno-setup-per-user-install.md) / [ADR-0008](adr/0008-feature-toggles-and-upgrade-policy.md)。

| 阶段 | 交付 | 证据 |
| --- | --- | --- |
| M1（2026-11-03） | `.apk` 缩略图闭环：CMake/Ninja（`/MT`、`/W4 /WX`）、`apkthumb.dll`（只导入系统 DLL）、HKCU 免 UAC 注册、自建正/负缓存、`thumbtest.exe` | 100 个包冷 P50 8.2 ms / P95 14.9 ms，热 P50 1.3 ms；真实 Explorer 管线冷 12–21 ms |
| M2（2026-11-04） | `apkinfo.exe --json`（schema v1）、ARSC 分段流式、签名块 v1/v2/v3、`--hash=sha256`、退出码、8 s watchdog | 与 `aapt2 dump badging` 11/11、与 apksigner 22/22 一致 |
| M3（2026-11-04） | `apkprop.dll` 单页（原生控件 + 可折叠分组）、`AddPages` 零 IO → Job 内 worker → `WM_APP` 回填 | 7.9 MB 包 140–188 ms 出全页；1.79 GB 包 157 ms |
| M4（2026-11-04） | `.xapk/.apks/.apkm`（Stored base 走零拷贝窗口）+ 鸿蒙 `.hap` | 1.25 GB 的 PUBG 只读 107 MB；HAP 2/2 |
| M5（2026-11-04） | Inno Setup 安装器（默认按用户、零 UAC）、三枚格式图标、VERSIONINFO、中英双语、命令并入 `apkinfo.exe` | 装/卸快照 diff 为 0；属性页中英各一张 |
| 收口后 | 图标 v2、功能开关（`thumb`/`icon`）、撞车询问、控制面板图标/发布者 | R-0015 / R-0016 / R-0017 |

**两个值得记住的意外**：

- 缩略图 handler 跑在 `dllhost.exe` 隔离宿主里，且只通过 `IInitializeWithStream` 喂数据
  （只实现 `IInitializeWithItem` 时系统返回 `0x8004B200`）→ 解析核心改成数据源无关，
  见 [ADR-0005](adr/0005-isolated-thumbnail-host-and-stream-input.md)。
- 这个 build 在建页前先 `QI(IShellExtInit)`，缺了会被静默跳过 → 已补
  （[m3-property-page.md §14](m3-property-page.md)）。

## 8. 下一步（按价值排序）

1. **代码签名**（OV/EV 待定，见 design.md §8）：v1 无证书，SmartScreen 会提示"未知发布者"。
2. **`/ALLUSERS` 整机安装再跑一轮验收**：`.iss` 的提权分支只在人工安装时走过，自动化里弹 UAC 会卡会话。
3. **修 `proptest.exe`**：本机（Windows 11 26300）`PropertySheetW` 立即返回 −1，换 M4 旧 DLL 同样失败；
   在修好之前，属性页验收用真实资源管理器对话框截图。
4. **ProcMon 复跑 V1/V9**：M5 没改缩略图代码路径、`DefaultIcon` 纯静态，但仍应测一次"0 次 CreateFile"。
5. HAP 增强（非阻塞）：layered icon 合成、`$media:<id>` 的 id→文件名映射、`.hsp`/`.app` 套件、HAP 签名块。
6. TD-002（deflate 的大 arsc）；V8 恶意样本回归；长跑（句柄 / 负缓存过期）。
7. 用户已明确挂起（2026-11-04）：真实 150%/200% DPI 人眼确认。

## 9. 接力说明（下一会话从这里开始）

当前状态一句话：**M0–M5 收口并已公开发布**——仓库 https://github.com/MWTJC/apk-shell-ext
（public，`main`），首个 release `v1.0.0` 的安装包已上传；可装可用、装/卸零残留已验证，剩下的都是"锦上添花 + 补测"。

- **构建**：`tools\build.cmd Release`（MSVC 自动发现，见 §2）→ `out\x64\`；`tools\build.cmd Release arm64` → `out\arm64\`。
- **打包**：`tools\pack.cmd`（会先跑 `sync-version.ps1`，再找 ISCC）→ `out\dist\`。
- **版本号**：只改 `src/version.h`；`sync-version.ps1 -Check` 能一眼看出包版本有没有漂。
- **夹具**：`tests\gen_*.py` 可随时重造（不入库）；真实样本在 `tests\fixtures\real\`（gitignore），
  换机器要重新准备：3 个普通 APK、2 个 `.xapk`（Poweramp 22 MB；PUBG 1.25 GB）、
  2 个 `.hap`、`big\`（1.26 G / 1.92 G）。
- **验收全套**：见 §5，**加上** `verify-install.ps1` 的装/卸/快照三步。
- **改图标/换 DLL 后**：删 `%LOCALAPPDATA%\IconCache.db` 与
  `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db` 并重启资源管理器，否则看的是旧图（踩过）。
- **图标是 v2（[R-0015](verification.md)）**：纸页 + 折角 + 色带 + 白字，几何常量在
  `tools\gen_icons.py` 顶部；改样式只动常量与 `FAMILIES`，再跑
  `uv run --with pillow python tools\gen_icons.py --preview`。字形是入库的 Nunito Black（OFL）。
  验"Explorer 实际画哪一档"用 `tools\icon_probe.ps1`（手工工具，不进构建）。
  **待定**：16 px 下 `.apk` 与 `.apks` 只差 2 px 台阶，是否给容器加独立标记。
- **功能开关（[R-0016](verification.md) / [ADR-0008](adr/0008-feature-toggles-and-upgrade-policy.md)）**：
  安装期 `[Tasks]` + `Check: AllowThumb/AllowIcon`（跳过的条目卸载时也不碰，实测过），
  运行期 `apkinfo --features`；`[UninstallRun]` 在删文件前跑 `--features all=off`。
  **改这块先看**：`.iss` 的 `AskAboutConflicts`、`support.cpp` 的 `RunFeaturesCommand`、
  `verify-install.ps1 -Features`。
- **改文案**：界面文案只改 `src/core/i18n.cpp` 的表；解析器错误保留 core 里的中文原文，
  由 `TrMsg` 字典在 worker 出口翻译。`propsheet_model_test --selftest` 会钉住中文断言。
- **1.0.1（[R-0019](verification.md)）属性页两处修复**：① 页头图标改走 `core/package.h` 的
  `ReadPackageIcon`（`.xapk/.hap` 以前没图标，因为退路只认根上的 `AndroidManifest.xml`）；
  ② `*\shellex\PropertySheetHandlers` 上的调试残留会让**每个文件**都多出一页——`register.ps1` 安装/卸载
  都会清（只删值等于我们自己 CLSID 的键），`verify-install.ps1 -Phase check` 会断言它不存在，
  `apkinfo --status` 会打印「全域注册：有/无」。
- **1.0.2（[R-0020](verification.md) / [ADR-0009](adr/0009-rename-in-place-upgrade.md)）升级不再关闭 Explorer**：
  `.iss` 改成 `CloseApplications=no`，`[Code]` 在 `ssInstall` 把被加载的二进制改名成 `*.old` 让位，
  `ssPostInstall` 清理、`DeinitializeSetup` 失败回滚。实测替换 157 ms、explorer PID 不变。
  **改这块先读 ADR-0009**：新增的 `.old` 文件是预期产物（卸载/下次安装清），别当成残留。
  注意安装包里的 `docs/install.md` 是**打包时**的快照，改完文档要重新 `pack.cmd` 才会进安装包。
  **1.0.3 已由 CI 发版**（tag `v1.0.3` → run `#42485690147` 四段全绿，Release 里两个安装包 + `SHA256SUMS.txt`，
  见 [R-0022](verification.md)）。1.0.2 只有本机安装记录，未发版。
  Arm64 包的验证程度是"CI 原生 Arm64 上编译 + 自测 + 缩略图解出 + 在本机 x64 上被架构守卫拒绝"，
  **壳集成（属性页 / 真实 Explorer 管线）仍未在 Arm64 实机验收**——用户目前没有 Arm64 设备，已明确接受"等反馈再说"。

## 10. 变更纪律

1. 每条技术断言要能指到出处：实测（二进制/注册表/ProcMon）、官方文档或对方源码。文档里不写"据说"。
2. 任何影响调用频率或进程边界的改动，先写 ADR 再改代码。
3. 每阶段结束在 [verification.md](verification.md) 追加一条记录（含命令与结论），未验证项单列，不许含糊。
4. 安装/卸载行为属于交付契约：改了必须同步 [install.md](install.md)（它会随安装包发给用户）
   与 `verify-install.ps1`。
5. 发布流程见 [release.md](release.md)（CI 接管后见 §11）。

## 11. CI 与本地自检（act）

CI 形态见 [ADR-0011](adr/0011-ci-on-windows-runners.md)，工作流是 `.github\workflows\build.yml`：

| job | 跑在哪 | 做什么 |
| --- | --- | --- |
| `build` | `windows-latest`（x64）+ `windows-11-arm`（arm64） | 版本号漂移闸门 → `tools\build.cmd Release <arch>` → PE 机器码断言（`0x8664` / `0xAA64`）→ `propsheet_model_test --selftest` → `thumbtest` 跑入库样本 `tests/fixtures/ci`（断言 count=5 / failed=1 / coldP95 ≤ 300 ms）→ 上传三个二进制 |
| `package` | `windows-latest` | 下载两个架构的产物到 `out\x64` / `out\arm64` → `pack.cmd x64` / `pack.cmd arm64` → `SHA256SUMS.txt` |
| `release` | `windows-latest`，仅 `v*` tag | `gh release create` 两个安装包 + 校验和 |

触发只有两种：Actions 页面手动 `workflow_dispatch`，或 `git push` 一个 `v*` tag。**没有 PR 门禁。**

当前状态（2026-11-08）：`#42483326344` 全绿——两条腿编译 + PE 断言 + selftest + thumbnail smoke 通过，
`package` 出两个安装包与 `SHA256SUMS.txt`。数字与未验项见 [verification.md R-0021](verification.md)。

**本地自检（nektos/act）**：act 只能跑 Linux 容器；Windows 腿要用 `-self-hosted` 落到本机（不经过 Docker）：

```bat
act -l                                                        :: 看 job 图
act -n workflow_dispatch                                      :: 只校验 YAML/表达式（Windows 腿会报"unsupported platform"，正常）
act workflow_dispatch -j build -P windows-latest=-self-hosted ^
    -P windows-11-arm=-self-hosted --matrix arch:x64          :: 真编译 + 真断言
```

实测（2026-11-08，act 0.2.97）：`-self-hosted` 下 build 腿能完整跑通（版本闸门 → 编译 →  三个 PE 断言 → selftest 全绿）。

**已知限制（踩过）**：

- `actions/upload-artifact@v4` 在 act 里会失败：`Unable to get the ACTIONS_RUNTIME_TOKEN env variable` —— artifact 服务是 GitHub 侧的，act 模拟不了。
  所以本地只能验到"编译 + 断言"，`package` / `release` 两段必须在 GitHub 上首跑。
- act 首次要 clone `actions/*`，国内网络需给 act 传代理：`HTTP_PROXY/HTTPS_PROXY=http://127.0.0.1:7897`
  （clone 缓存在 `%USERPROFILE%\.cache\act`）。
- `--matrix arch:x64|arm64` 只跑一条腿；不带时两条腿都会在映射到的同一台机器上跑（arm64 腿会走本机交叉编译，能验逻辑但验不了"原生 Arm64 runner"）。
- 跑 act 前先删掉 `build\` / `build-arm64\`：`CMakeCache.txt` 里存的是绝对路径，act 会把工作区复制到
  `%USERPROFILE%\.cache\act\<hash>\hostexecutor\`，旧缓存与新路径不一致会直接报
  "The source ... does not match the source ... used to generate cache"（踩过）。
- arm64 腿里的 `Self-test` 在 x64 主机上必然失败（x64 跑不了 `AA64` 的 exe）；这一步的本地意义只有"编译 + 断言"，真正的 selftest 要在 `windows-11-arm` 上跑。
- `thumbtest` 对**预期失败**的样本（如 `f_noicon.apk`）返回 **2**，而 GitHub 的 `shell: pwsh` 外壳会在末尾
  `exit $LASTEXITCODE` —— 断言全过也会被判红。`Thumbnail smoke` 步骤因此以显式 `exit 0` 收尾，判据只留断言（踩过）。
- 托管镜像的 WebP 支持不一致（`[实测]` #42482442617）：`windows-latest` 没有 WIC WebP 编解码器
  （`f_webp` 56 ms 即判失败），`windows-11-arm` 有但冷启动 798 ms。所以 smoke 断言按"结构 + 热路径预算"判，
  冷路径只用 1500 ms 兜底 —— 别再把 `coldP95 ≤ 300` 写回去。
- **CI 绿 ≠ 验收绿**：ProcMon 零读取、300 ms 预算、装/卸零残留、Arm64 运行期行为仍按 [verification.md](verification.md) 人肉验。
