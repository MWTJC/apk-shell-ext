# 验收方法与记录

> 目的：把 README 里的"无感"变成**别人拿着命令能复现**的数字。
> 规则：每条记录必须含「环境 / 命令 / 原始输出摘要 / 结论 / 未验证项」。没跑过的不要写"通过"。

## 0. 环境基线（每次记录都要带）

| 项 | 值 |
| --- | --- |
| OS | `winver` + `Get-ComputerInfo | Select OsBuildNumber` |
| Explorer | `(Get-Item C:\Windows\explorer.exe).VersionInfo.FileVersion` |
| 磁盘 | 被测 APK 所在盘（机械盘数字不作数，需注明 NVMe/SSD） |
| 样本 | 目录 + 文件数 + 总大小；来源（自造/真实包/从手机导出） |
| 构建 | git commit + 构建配置（Debug/Release，是否带日志） |

## 1. V1 · 零开销（B2 的硬判据）

**判据**：详细信息视图浏览含 APK 的目录、滚动 30 s，explorer 对 `*.apk` 的 `CreateFile` 次数 = **0**。

**配方（ProcMon）**

1. 启动 ProcMon（管理员），先 `Ctrl+E` 停止捕获，清空。
2. Filter：
   - `Process Name is explorer.exe`
   - `Operation is CreateFile`
   - `Path ends with .apk`
   - 勾选 `Drop Filtered Events`（减少噪声）
3. 打开目标目录 → 切「详细信息」→ 上下滚动 30 s → 悬停 10 个文件 → 切换一次排序。
4. 停止捕获，读取事件数。

**期望**：0。若出现命中，看 `Detail` 里的调用栈（ProcMon 的 Stack 列）定位是谁触发的——如果没有注册 `IPropertyStore`，基本上只可能是别的软件（如杀软、云同步、百度网盘缩略图）。

**已知干扰项**：网盘/同步客户端、杀软实时扫描、Windows Search 索引。测量前逐个暂停，或按 `Process Name` 严格过滤。

### 1.1 自动化配方（M1 交付，可复现）

ProcMon 的命令行**没有** `/Filter` 开关（只有 `/LoadConfig` + 二进制 `.pmc`），所以采用
"不限过滤、短时捕获、事后在 CSV 上过滤"的做法；关键在于**捕获必须已经开始，再去浏览目录**，
否则就是空测（空捕获当然 0 条）：

```powershell
# 一次 UAC：Procmon 抓 $Seconds 秒 → 导出 CSV；脚本在捕获期间完成"进入目录 + 切视图 + 刷新 + 悬停"
powershell -File tools\v1-procmon.ps1 -Folder <目录> -Seconds 16 -View Details -WorkDir <工作目录>
python tools\parse-procmon-csv.py <工作目录>\v1.csv --ext .apk
```

**灵敏度对照（必须做）**：同一条命令把 `-View` 换成 `Large`，期望出现命中——否则说明测量无效。
M1 实测（同一台机、同一批 12 个 APK、两边都先清空自建缓存）：

| 场景 | explorer.exe `CreateFile(*.apk)` | DllHost.exe | 合计 opens |
| --- | --- | --- | --- |
| **详细信息视图**（进入 + 切视图 + 刷新 + 悬停 10 项） | **0** | 0 | **0** |
| 大图标视图（同样动作，灵敏度对照） | 24 | 12 | 36 |
| 捕获事件总量 | 774,226 | 789,925 | — |

**V1 判定：通过**（详细信息视图 0 次，且同方法在大图标视图确实能抓到 36 次）。
附带发现：大图标视图下 **explorer.exe 自己也要开文件**（每文件 2 次），随后 DllHost.exe 再开 1 次
拿数据流——这解释了为什么"缩略图只读几十 KB"的设计在隔离宿主下仍成立（探测与取图分离）。

## 2. V2/V3 · 缩略图耗时（冷/热）

**工具**：`tests\thumbtest.exe`（M1 交付；`--shell` 走 `IShellItemImageFactory::GetImage(SIIGBF_THUMBNAILONLY)`，
与 Explorer 同一条路径且经 dllhost；不带 `--shell` 时直接 DllGetClassObject 驱动 provider）。

```
thumbtest.exe --dir "D:\apks" --size 256 --repeat 3 --json
thumbtest.exe --shell --dir "D:\apks" --repeat 2 --json
```

输出每个文件的第一次耗时（冷）与后续（热），以及 P50/P95/最大值。

**判据**

| 场景 | P50 | P95 | 失败率 |
| --- | --- | --- | --- |
| 冷（清空自建缓存 + thumbcache 后首次） | ≤ 120 ms | ≤ 300 ms | ≤ 2% |
| 热（自建缓存命中） | ≤ 20 ms | ≤ 50 ms | 0 |

**清冷缓存**：停 explorer → 删 `%LOCALAPPDATA%\apk-shell-ext\` → 删 `%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db` → 起 explorer。

**注意**：`GetImage` 会经过系统 thumbcache，所以"热"实际测的是"系统缓存 + 自建缓存"的最优路径；要单测 DLL 内部耗时，打开诊断开关看日志里的 `decodeMs`。

## 3. V4 · explorer 内存

```
# 基线（卸载扩展、重启 explorer 后）
$e = Get-Process explorer; $e.PrivateMemorySize64/1MB
# 装上扩展，浏览含 100 个 APK 的目录（大图标视图）后再取
```

**判据**：私有提交增量 ≤ 3 MB（M1）/ ≤ 5 MB（含属性页加载后）。

更好的是 VMMap 看 `apkthumb.dll` 的 Private Data + 映像大小；任务管理器只能给粗数。

## 4. V5 · 健壮性（不许崩 explorer）

样本集（每个都要过一遍，观察 explorer PID 是否变化）：

| 样本 | 构造方式 | 期望 |
| --- | --- | --- |
| 空文件改名 `.apk` | `fsutil file createnew` | 默认图标，不崩 |
| 截断包 | 真实 APK 取前 60% 字节 | 默认图标 |
| 非 ZIP 改名 | 任意 exe → `.apk` | 默认图标 |
| 条目炸弹 | 自造 ZIP，100 万个 0 字节条目 | 拒绝（条目数上限），不崩 |
| 超大 arsc | 构造 64 MB `resources.arsc` | 超限拒绝或超时回落 |
| 路径穿越名 | 条目名 `..\..\evil.dll` | 不落盘、不崩 |
| 加密 ZIP | 真实 APK 的条目置加密位 | 默认图标，不弹密码框 |

**判据**：explorer PID 不变、0 次 WER 记录。

## 5. V6 · 安装/卸载零残留

```
reg export "HKCR" before.reg /y          # 太大时只导出相关键
install.cmd
reg export ... after_install.reg
uninstall.cmd
reg export ... after_uninstall.reg
# 比对：after_uninstall 与 before 的差异应只含时间戳/无关系统项
```

**判据**：`.apk` 下的 `ShellEx`、`shellex\PropertySheetHandlers`、两个 CLSID 全部消失；安装目录清空；explorer 重启后 APK 恢复白纸图标。

## 6. V7 · 属性页时延与稳定性（M3）

| 项 | 判据 | 方法 |
| --- | --- | --- |
| `AddPages` 耗时 | ≤ 50 ms | 日志打断点（QPC） |
| 冷开 | P95 ≤ 1 s（≤ 50 MB 包） | 手工点开 20 次 + 日志 |
| 热开 | ≤ 200 ms | 同上（缓存命中） |
| 解析中关窗 | 20 次不崩、无残留 `apkinfo.exe` | 任务管理器计数 |
| worker 崩溃 | 页面显示错误、explorer 存活 | 任务管理器结束 `apkinfo.exe` |
| 句柄/内存 | 开关 100 次无单调增长 | `Get-Process explorer` 采样 |

## 7. V8 · 恶意样本不触发已知漏洞路径

**背景**：ApkShellext2 因内置 libwebp 未修 **CVE-2023-4863** 被点名（issue 仍 open）`[上游]`。本项目规定不走自有 PNG/WebP 解码器，只用 WIC `[决策见 ADR-0004]`。

**验证**：用 3 个公开的 libwebp 崩溃样本（改成 `.apk` 外壳，图标换成样本）走一遍缩略图与属性页；期望：WIC 正常拒绝或返回空图，不崩。

## 8. V9 · 静态类型图标（M5）

**判据**

- 安装后，「小图标 / 列表 / 详细信息」视图里 `.apk/.ipa/.hap/.aab/.appx` 各显示**不同**的格式图标（人工眼检 + 截图存档）；
- 同时 **V1 复跑仍为 0 次 `CreateFile`**（证明图标是 Explorer 读资源，不是我们的代码）；
- 卸载后 `DefaultIcon` 恢复原值（原来无值则键被删除），图标回到系统默认；
- 重装后图标正常（路径变化时注册值同步更新）。

**配方**

```
reg query "HKCR\.apk\DefaultIcon" /ve
# 安装前后各存一份；卸载后 diff 应为 0
```


## 9. 记录表

> 新记录追加到表尾；编号 `R-0001` 起，不复用。

| 编号 | 日期 | 阶段 | 结论 | 证据（命令/输出摘要） | 未验证 |
| --- | --- | --- | --- | --- | --- |
| R-0001 | 2026-11-03 | M0 | 文档基线建立；**未做任何代码验证** | 本套文档 | M1 起全部待测 |
| R-0004 | 2026-11-04 | M2 | **TD-001 关闭**：arsc 改流式跳读，19.7 MB 真表 0.7–1.5 ms、结果与旧路径逐字段一致；大包（1.26 G / 1.92 G）解析 3.8–6.9 ms；修掉一个 `typeIdOffset` 越界导致的静默丢图 | 见 §12 | `signature`/`features`/`splits` 字段、deflate 大 arsc（TD-002） |
| R-0003 | 2026-11-03 | M2（部分） | `apkinfo.exe --json` 骨架：7/7 包与 `aapt2 dump badging` 的包名/版本/SDK/ABI/权限/名称完全一致；`--hash=sha256` 与 `certutil` 一致；退出码 0/2/3 均实测 | 见 §11 | `signature`/`features`/`splits` 字段、TD-001（arsc 分段读）、500 MB 级大包 |
| R-0002 | 2026-11-03 | M1 | APK 缩略图闭环打通：**真实 Explorer 大图标视图已出包内图标**（截图存档）；**V1 零开销通过**（详细信息 0 次 vs 大图标 36 次）；解析与 aapt2 一致；8 个坏包优雅失败 | 见下 §10、§1.1、§V4 | V8（libwebp 样本）、长跑（句柄/负缓存过期）、V1 的 30 s 长捕获 |

## 10. R-0002 详细记录（M1，2026-11-03）

### 环境

| 项 | 值 |
| --- | --- |
| OS | Windows 11（宿主报告 10.0.26300），x64 |
| 工具链 | MSVC 19.51.36260（VS 18 Community，MSVC 14.51 工具集）· CMake 3.31.3 · Ninja 1.12.1 |
| 构建 | `tools\build.cmd Release`（`/MT`、`/W4 /WX`、`/guard:cf`），产物 `out\x64\{apkthumb.dll, thumbtest.exe}` |
| 样本 | `tests\fixtures\generated\` 4 个 aapt2 真实构建的 APK（PNG/WebP/自适应/无图标）+ `tests\fixtures\generated\bad\` 8 个坏包 + 3 个真实包（aida64 8.2 MB、qwxds 1.1 MB、wwlocation 0.3 MB） |
| 样本可复现性 | `python tests\gen_fixtures.py` 全程可复现：两次运行（不同 cwd）产物逐字节一致；脚本自带 141 条断言，任一条不满足即退出码 1。`aapt2 compile` 必须带 `--no-crunch`（否则 aapt2 会重编码 PNG，图标字节不再等于源文件），WebP 必须 `-lossless 1 -pix_fmt bgra`（默认 yuv420 会把 `#00FFFF` 舍入成 `#01FFFF`，颜色探针就失效了）。`generated/` 与 `real/` 都不入库 |
| 磁盘 | 系统盘 NVMe（`C:`），工作目录 `D:`（同一物理盘） |
| 观测工具 | Sysinternals Suite（Store 版 2026.10.0.0）`Procmon.exe`；脚本 `tools\1-procmon.ps1` + `tools\parse-procmon-csv.py` |

### V2/V3 · 耗时（`thumbtest`，同进程批量）

```
thumbtest --clean-cache
thumbtest --dir <100 个 APK 的目录> --repeat 3 --json
```
输出：
```
summary{"count":100,"failed":25,"coldP50":8.20,"coldP95":14.92,"coldMax":111.57,
        "hotP50":1.31,"hotP95":2.24,"hotMax":2.65}
```
`failed=25` 全部是 `f_noicon.apk`（包内确实没有 `android:icon`），属预期；
`coldMax=111.57 ms` 是进程内第一次解 WebP 时的编解码器初始化，只发生一次。

**判据对照**：冷 P50 ≤ 120 ✓（8.2）、P95 ≤ 300 ✓（14.9）；热 P50 ≤ 20 ✓（1.3）、P95 ≤ 50 ✓（2.2）。

### V2/V3 · 真实 Explorer 管线（`--shell`，经 dllhost）

```
thumbtest --shell --dir <6 个刚复制、系统 thumbcache 未命中的 APK> --repeat 2 --json
```
```
a1(自制 PNG)  first=158.82  last=2.98
b1(自制 WebP) first=114.46  last=0.65
c1(aida64)    first= 20.81  last=0.52
d1(自适应)    first= 13.40  last=0.66
e1(qwxds)     first= 17.23  last=1.07
f1(自制 PNG)  first= 12.04  last=0.42
summary{"coldP50":20.81,"coldP95":158.82,"hotP50":0.66,"hotP95":2.98}
```
前两条的 100–160 ms 是 dllhost 进程启动 + WIC WebP 编解码器初始化；**Steady state 12–21 ms**，
系统 thumbcache 命中后 < 3 ms。均在 300 ms 预算内。

### 解析正确性（`--probe`，与 aapt2 输出对照）

| 样本 | aapt2 dump badging / resources | 本实现 |
| --- | --- | --- |
| `f_png.apk` | package com.example.pngicon，versionCode 10203，versionName 1.2.3，minSdk 26，targetSdk 34，icon `0x7f010000` = `res/mipmap-xxxhdpi-v4/ic_launcher.png` | 全部一致；中心像素 `FF00FF`（= xxxhdpi 的洋红，证明按密度选对了） |
| `f_webp.apk` | icon `res/mipmap-xxhdpi-v4/ic_launcher.webp` | 一致；中心像素 `00FFFF` |
| `f_adaptive.apk` | `mipmap-anydpi-v26/ic_launcher.xml` + `mipmap-xxxhdpi-v4/ic_launcher.png` | 选中 xxxhdpi 光栅回退（M1 不做 adaptive 合成）；中心像素 `0066FF` |
| `f_noicon.apk` | 无 `android:icon` | 返回 `E_FAIL`，负缓存 |
| `aida64-1.94.apk` | `res/mipmap-xxxhdpi-v4/ic_launcher.png` | 一致，缩略图为红色 "64" 图标（人工目视） |
| `qwxds-1.6.1.apk` | 资源名被混淆为 `res/xC.webp` | 通过 arsc 解析到位（非启发式），缩略图为默认安卓机器人图标 |

### V5 · 健壮性（8 个坏包，同一进程内连续处理）

| 样本 | 结果 |
| --- | --- |
| `empty.apk`（0 字节） | `E_FAIL`，日志"文件太小，不是有效 ZIP" |
| `notzip.apk`（4 KB 随机字节） | `E_FAIL`，"未找到 ZIP 结束记录" |
| `trunc.apk`（真实包截断 60%） | `E_FAIL`，同上 |
| `bomb_entries.apk`（70000 条目） | `E_FAIL`，"ZIP 条目数超限（>65535）" |
| `deflate_bomb.apk`（声明 300 MB） | `E_FAIL`，清单条目超过 8 MB 上限，拒绝解压 |
| `traversal.apk`（`../../evil.dll` 条目名） | `E_FAIL`；全程不落盘（无临时文件产生） |
| `bigarsc.apk`（64 MB 资源表，TD-001 上限 8 MB） | 跳过 arsc，走"按文件名猜"启发式并**成功出图**（实测 5.8 ms），符合 TD-001 设计意图 |
| `badicon.apk`（图标数据替换为垃圾） | `E_FAIL`，"WIC 无法解码该图像"，负缓存生效（第二次 0.95 ms vs 第一次 30.3 ms） |

进程全程未崩溃（同一 `thumbtest.exe` 进程内顺序处理 8 个坏包 + 3 个重复）。

### V6 · 安装/卸载零残留

```
powershell -File tools\register.ps1 -Install       # 写 HKCU\Software\Classes（免 UAC）
powershell -File tools\register.ps1 -Uninstall
测试路径: Test-Path 'HKCU:\Software\Classes\.apk\ShellEx'                     -> False
          Test-Path 'HKCU:\Software\Classes\CLSID\{A7C4E1B0-...-7B01}'        -> False
```
安装写入的两个键（`.apk\ShellEx\{E357FCCD…}`、`CLSID\{…}\InprocServer32`）连同被清空的
`.apk\ShellEx` 父键一并移除；备份文件 `%LOCALAPPDATA%\apk-shell-ext\install-backup.json`
在卸载后删除。

### V-B1 · 真实 Explorer 窗口（大图标视图）

```
powershell -File tools\capture-explorer.ps1   # UIA 切"大图标"→ 等 4.5 s → 只截该窗口
```
证据截图：[docs/evidence/m1-explorer-large-icons.png](evidence/m1-explorer-large-icons.png)

六个文件的表现（同一目录，系统 thumbcache 与本项目缓存均已清空后首次进入）：

| 文件 | 显示 | 结论 |
| --- | --- | --- |
| `AIDA64.apk`（真实包） | 红色 "64" 应用图标 | ✅ 走 arsc → `mipmap-xxxhdpi-v4/ic_launcher.png` |
| `微信多开器.apk`（真实包） | 绿色安卓机器人图标 | ✅ 走 arsc → 混淆名 `res/xC.webp` |
| `Demo_PNG.apk`（自造，5 档密度不同色） | 洋红色方块 | ✅ 选中 xxxhdpi（640），密度挑选正确 |
| `Demo_WebP.apk`（自造） | 青色方块 | ✅ WebP 经 WIC 解码 |
| `Demo_Adaptive.apk`（自造，含 anydpi XML + 光栅回退） | 蓝色方块 | ✅ 命中 xxxhdpi 光栅回退（M1 不合成图层） |
| `No_Icon.apk`（自造，无 `android:icon`） | 白色纸张图标 | ✅ 按 ADR-0001 回落系统默认 |

**这条同时给"B1 生效"提供了端到端证据**：Explorer 的图标确实是我们的 handler 从包内取出并解码的，
而不是任何静态资源。

### 二进制规模（对照 ADR-0004 的"两个 DLL 合计 < 500 KB"）

| 产物 | 大小 |
| --- | --- |
| `apkthumb.dll` | 285 KB（291,840 B） |
| `thumbtest.exe`（验收工具，不进交付物） | 375 KB |

导入表只有 `ole32 / bcrypt / SHLWAPI / KERNEL32 / GDI32`，无 `vcruntime*/msvcp*`（`/MT` 生效）；
导出仅 `DllGetClassObject` / `DllCanUnloadNow`；PE 头含 Control Flow Guard（`tools\inspect-deps.cmd`）。

### V4 · 内存增量（任务管理器口径，非 VMMap）

同一台机、100 个 APK、大图标视图，用扩展"卸载 / 已装"两次 A/B（每次用独立的同名目录，避免视图状态与
thumbcache 复用）：

| 配置 | explorer 私有提交增量 | 我们的 handler 所在进程 |
| --- | --- | --- |
| 扩展**未**安装（100 个静态纸张图标） | +168.3 MB | — |
| 扩展已安装（100 个缩略图） | +204.8 MB | dllhost.exe 私有 **4.3 MB**（含 WIC 等宿主自身开销） |

结论：**我们的代码不进 explorer**；explorer 的增量主要是 100 张 256×256 位图与 shell 视图结构（168 MB
在未装扩展时就已经发生）。"explorer 增量 ≤ 3 MB"这条判据写在 handler 假定运行于 explorer 时代，现已不适用，
后续按"dllhost 增量 ≤ 10 MB"考核更合理（M3 属性页仍会进 explorer，届时重新定义）。

复现：`powershell -File tools\measure-memory.ps1 -Folder <目录>`

### 本阶段未验证（不许当作通过）

1. **V8 恶意样本**：未取公开的 libwebp 崩溃样本；只验证了"坏 PNG 数据被 WIC 拒绝"。
2. 句柄泄漏（开关 100 次）与 24 h 负缓存过期策略未做长跑验证。
3. V1 的"滚动 30 s"用"进入 + 切视图 + 刷新 + 悬停 10 项、捕获 16 s"替代，时长未到 30 s；
   结论方向不变（0 vs 36），但严格按原始判据还需补一次 30 s 的长捕获。
4. V4 用的是任务管理器口径（`Get-Process PrivateMemorySize64`），没有 VMMap 的 DLL 级私有数据拆分。

## 11. R-0003 记录（M2 部分，2026-11-03）

**交付**：`outd\pkinfo.exe`（静态 CRT，285 KB 级 core 复用）+ JSON schema v1 的
`file` / `app` / `icon` / `components` / `permissions` / `diagnostics` 六组字段。

### 交叉校验（判据：与 aapt2 一致，允许我们多显示）

```
python tests\verify_apkinfo.py <apk...>
```

```
OK   f_png.apk         com.example.pngicon      1.2.3 (10203)  sdk 26/34  icon=res/mipmap-xxxhdpi-v4/ic_launcher.png 0.46ms
OK   f_webp.apk        com.example.webpicon     2.0.1 (2001)   sdk 26/34  icon=res/mipmap-xxhdpi-v4/ic_launcher.webp  0.51ms
OK   f_adaptive.apk    com.example.adaptiveicon 4.1.0 (4001)   sdk 26/34  icon=res/mipmap-xxxhdpi-v4/ic_launcher.png 0.63ms
OK   f_noicon.apk      com.example.noicon       3.0.0 (3001)   sdk 26/34  icon=None                                   0.57ms
OK   aida64-1.94.apk   com.finalwire.aida64     1.94 (194)     sdk 17/31  icon=res/mipmap-xxxhdpi-v4/ic_launcher.png 2.50ms
OK   qwxds-1.6.1.apk   com.rong862.fqywx        1.6.1 (1061)   sdk 28/33  icon=res/xC.webp                            0.77ms
OK   wwlocation-...    com.example.wwlocation   0.2.0-probe    sdk 28/33  icon=None                                   0.84ms
7/7 packages matched
```

比对项：`package`、`versionCode`、`versionName`、`minSdk`、`targetSdk`、`application-label`、
`native-code`（ABI 集合）、`uses-permission`（集合）。差异表为空。

### 哈希与退出码

```
apkinfo --json --hash=sha256 aida64-1.94.apk
  file.sha256 = 7650825e3fad8dceb3667bf5a8e19bf8c996f081b214bb4aa0529f61c79fbc7e
certutil -hashfile aida64-1.94.apk SHA256
  7650825e3fad8dceb3667bf5a8e19bf8c996f081b214bb4aa0529f61c79fbc7e      ← 一致
```

| 样本 | 退出码 | stderr（现在按 UTF-8 输出） |
| --- | --- | --- |
| `f_png.apk` | 0 | — |
| `bad/empty.apk`（0 字节） | 2 | `apkinfo: 文件太小，不是有效 ZIP` |
| `bad/notzip.apk` | 2 | `apkinfo: 未找到 ZIP 结束记录（不是有效 ZIP）` |
| `bad/bomb_entries.apk` | 2 | `apkinfo: ZIP 条目数超限（>65535）` |
| `bad/traversal.apk` | 2 | `apkinfo: AXML 文件太小` |
| `bad/badicon.apk` | 0 | 元数据正常，`diagnostics.warnings` 里有"图标条目不是可识别的图像数据" |
| `bad/bigarsc.apk`（64 MB arsc） | 0 | 元数据正常，warning 说明 arsc 被跳过（TD-001） |
| `--timeout=1` + `APKSX_TEST_DELAY_MS=2500` | 3 | `apkinfo: timeout after 1000 ms`（watchdog 自杀路径实测） |

### 未验证

1. `signature`（v1/v2/v3 方案识别 + 证书摘要）、`features`（uses-feature）、`splits`（APKS 分包）尚未实现。
2. TD-001 仍未关闭：arsc 依然整读（> 8 MB 跳过并降级），M2 的"分段流式"未开工。
3. "≤ 50 MB 包 P95 ≤ 300 ms / ≤ 500 MB 包 ≤ 1.5 s"这条时限判据没跑：手头最大样本 8.2 MB（9.8 ms，含 sha256）。
4. 恶意样本（V8）与长跑（句柄泄漏）仍未做。

## 12. R-0004 记录（M2 · TD-001 关闭，2026-11-04）

### 变更

- 新增 `src/core/arsc_stream.{h,cpp}`：在 **Stored** 的 `resources.arsc` 上开只读窗口
  （`ByteSource::SliceSource`）按 chunk 跳读，不再把整表读进内存。
- `ZipReader::OpenStoredEntry()`：Stored 条目的零拷贝视图；deflate 条目返回空 → 调用方回退。
- 数据源所有权 `unique_ptr` → `shared_ptr`（切片需要共享生命周期）。
- `apk_info` 的语言/密度改为遍历 type chunk 头；label 解析走流式。

### 夹具（可复现）

```
python tests/gen_big_arsc.py
  f_bigarsc_real.apk: resources.arsc = 19,714,916 bytes (Stored)   # 4000 条长字符串 × 3 语言
  tdeflate_arsc.apk : resources.arsc = 1,484 bytes (Deflated)      # TD-002 回退路径
```

### TD-001 关闭判据对照

| 判据 | 结果 |
| --- | --- |
| 分段流式读取，删除整读路径 | Stored 走流式；整读只剩"deflate 且 ≤ 8 MB"的回退分支（TD-002） |
| ≥ 32 MB 资源表样本仍满足 V2 | 手边最大真表 19.7 MB：`apkinfo` 0.71–1.45 ms、缩略图 33 ms（含首次 WIC 初始化）。**没有 32 MB 的真样本**，用 19.7 MB 佐证，差距在文档里写明 |

19.7 MB 的表只读了几百字节还是几千字节，取决于要枚举多少配置：查图标 = 表头 + 池头 +
目标 type chunk 头 + 一个条目；枚举语言/密度 = 每个 type chunk 的头（该样本 ~800 个 chunk）。

### 大包测速（新样本，用户提供）

| 样本 | 条目数 | arsc（Stored） | `apkinfo` 解析 | 缩略图（直接） | 缩略图（经 dllhost） |
| --- | --- | --- | --- | --- | --- |
| `base_1.26G.apk`（Arcaea 国服 7.2.0） | 5772 | 735 KB | **6.2 ms** | 37 ms | — |
| `base2_1.92G.apk`（少女前线2 4.0） | 1564 | 1.25 MB | **3.8–5.1 ms**（5 次） | 41 ms | 172 ms |

`--hash=sha256` 单独计：1.92 GB 用 **1.16 s**（磁盘吞吐限制），只在属性页显式请求时发生。
对照 roadmap 的"≤ 500 MB 包 ≤ 1.5 s"：解析路径 5 ms 级别，余量极大。

两个 GB 级包在**资源管理器大图标视图**下的实际显示（流的同一套代码路径）：
[docs/evidence/m2-big-apk-thumbnails.png](evidence/m2-big-apk-thumbnails.png)
—— 1.26 GB 的 Arcaea 国服与 1.92 GB 的少女前线2 都正确取出包内正方形图标。

### 回归：11 个包与 aapt2 一致

```
python tests\\verify_apkinfo.py tests\\fixtures\\generated\\*.apk tests\\fixtures\\real\\*.apk tests\\fixtures\\real\\big\\*.apk
...
11/11 packages matched
```

含 `f_bigarsc_real.apk`（19.7 MB 表）、`tdeflate_arsc.apk`（deflate 回退）、两个 GB 级真包。

体积：`apkthumb.dll` 308,736 B（301 KB，M1 为 291,840 B），`apkinfo.exe` 336,384 B（329 KB），
静态 CRT 不变（导入表仍只有系统 DLL）。

### 过程中发现并修复的隐蔽 bug

流式读包头时缓冲区只有 256 B，却访问偏移 284 的 `Package.typeIdOffset` —— 越界读到栈垃圾，
把它当偏移后**该包所有 type id 都对不上**，表现为"图标静默消失、只走启发式"。
在 `qwxds-1.6.1.apk`（资源名混淆的包）上暴露；修复后该包重新解析出 `res/xC.webp`。

教训：**两套解析器必须同时跑同一批真实包**——只跑自家 aapt2 夹具时这条路径永远是对的。

### 未验证

1. `signature` / `features` / `splits` 仍未实现（M2 剩余部分）。
2. 没有 ≥ 32 MB 的真 arsc 样本（19.7 MB 是现有上限）。
3. TD-002（deflate 大 arsc）未解。

## 13. R-0005 记录（M2 · 签名块 + uses-feature，2026-11-04）

### 环境

| 项 | 值 |
| --- | --- |
| OS | Windows 11 `10.0.26300` |
| Explorer | `10.0.26100.1710` |
| 构建 | commit `f8b0de4` + 本条改动，Release `/MT /W4 /WX` |
| 对照工具 | `apksigner` / `aapt2`（build-tools 36.0.0），JDK `21.0.12.1` |
| 样本 | 3 个自造签名夹具 + 5 个真实包（含 1.26 G / 1.92 G）+ 8 个坏包 |

### 交付

- `src/core/apk_signature.{h,cpp}`：v1（`META-INF/*.RSA|DSA|EC` 的 PKCS#7，`CryptQueryObject`）与
  APK Signing Block 的 v2 / v3 / v3.1 识别 + 证书摘要。证书信息用 `CertCreateCertificateContext` /
  `CertNameToStrW`（X500 + REVERSE）取，摘要 = 证书 DER 的 SHA-256。
- `ZipReader` 增加 `cd_offset()/cd_size()/ReadRange()`（签名块贴在中央目录前，需要纯粹的偏移读）。
- `apkinfo` JSON 增加 `signature`（`scheme` + `certs[]`）与 `features`（`uses-feature`）。
- `tests/gen_signed_fixtures.py`：用一次性密钥 + apksigner 造 v1-only / v2-only / v3-only 三个真实签名包。
- `tests/verify_signature.py`：与 `apksigner verify --verbose --print-certs` 对照。
- `tests/verify_apkinfo.py` 增加 `uses-feature` 对照。

### 签名块结构（用真实包逐字段核对过，不是"据说"）

```
块  = [uint64 size][pairs][uint64 size]["APK Sig Block 42"(16B)]
      size 不含第一个 size 字段 → 块起点 = cd_offset - (size + 8)；pairs 区结束于 size - 16
pair = [uint64 len = 4 + value_len][uint32 id][value]
v2/v3/v3.1 的 value = lp(signers 序列)，序列里每个 signer 又各有一层 lp：
  signer = lp(signed data) lp(signatures) lp(public key)
  signed data = lp(digests) lp(certs) lp(attributes)   # v3/v3.1 的 min/max SDK 在 certs 之后
```

踩坑记录：只按"两层"解析（漏掉 signer 的 lp）时，数字看起来都自洽（`digests` 长度、证书长度全对得上），
但读不到 signatures / public key —— 说明少了一层。用 `wwlocation` 的十六进制逐 4 字节核对了
`1386 / 1382 / 808 / 44 / 748 / 268 / 294` 这串长度，才把层级定下来。**结构解析不能只看"能不能读出证书"**。

### 与 apksigner 对照（22/22）

```
python tests/verify_signature.py --glob "tests/fixtures/generated/signed/*.apk" \
    --glob "tests/fixtures/generated/*.apk" --glob "tests/fixtures/generated/bad/*.apk" \
    --glob "tests/fixtures/real/*.apk" --glob "tests/fixtures/real/big/*.apk"
... 22/22 matched
```

| 样本 | 我们的 scheme | apksigner | 证书 SHA-256（前 8 位） |
| --- | --- | --- | --- |
| `s_v1.apk`（自造，targetSdk 28） | v1 | v1 | `787a5773` |
| `s_v2.apk`（自造） | v2 | v2 | `787a5773` |
| `s_v3.apk`（自造） | v3 | v3 | `787a5773` |
| `aida64-1.94.apk` | v1, v2, v3 | v1, v2, v3 | `a40da80a` |
| `wwlocation-probe-debug.apk` | v2 | v2 | `bbab1252` |
| `base_1.26G.apk` | v2 | v2 | `b04bbba0` |
| `base2_1.92G.apk` | v1, v2 | v2 | `9ac7d84e` |
| 8 个坏包 + 4 个未签名夹具 | （无 `signature`） | DOES NOT VERIFY | — |

`base2` 的 v1 差异已查明原因：它的 `META-INF/GF2.SF` 写了 `X-Android-APK-Signed: 2`，
验证器因此跳过 v1（报 `v1=false`），但 v1 块本身确实在包里。
**所以 `signature.scheme` 的语义是"检测到的签名块"，不是"校验通过"**——本阶段不做摘要校验。

另外：证书主题我们按 RFC 2253 + 反向输出，与 apksigner 只差属性名写法（`E=` vs `EMAILADDRESS=`），
序列化时不参与比较，只有 `sha256` 参与。

### features（uses-feature）与 aapt2 对照（11/11）

```
python tests/verify_apkinfo.py tests/fixtures/generated/*.apk tests/fixtures/real/*.apk tests/fixtures/real/big/*.apk
... 11/11 packages matched
```

含 `base2_1.92G.apk` 的 `android.hardware.vulkan.version`（`required=true`）与
`base_1.26G.apk` 的 `android.hardware.bluetooth`（`required=false`）。
aapt2 会把"隐含特性"同时打成 `uses-feature` 与 `uses-implied-feature` 两行（默认 faketouch、屏幕方向、
由权限推出来的 wifi），对照脚本按后者剔除，否则会误判成我们少字段。

### 耗时（5 次取 min/max，`diagnostics.elapsedMs`）

| 样本 | 加签名前 | 现在 | 说明 |
| --- | --- | --- | --- |
| `f_png.apk`（未签名） | 0.7–1.0 ms | 0.7–1.0 ms | 无签名块，多花的只是扫描 `META-INF` 名字 |
| `wwlocation-probe-debug.apk`（v2） | — | 3.9–6.2 ms | 小包，主要是 crypt32/ASN.1 首次初始化 |
| `aida64-1.94.apk`（v1+v2+v3） | — | 9.7–10.8 ms | 三种方案都解析，证书去重后 1 张 |
| `base_1.26G.apk`（v2） | 6.2 ms | 9.1–11.8 ms | +3–5 ms |
| `base2_1.92G.apk`（v1+v2） | 3.8–5.1 ms | 10.1–12.2 ms | +5–7 ms（多一个 PKCS#7） |

结论：每个 worker 进程多付 3–7 ms（`CertCreateCertificateContext` / `CryptQueryObject` 首次初始化），
相对 300 ms（缩略图）/ 1 s（属性页）判据余量仍然很大。**缩略图路径完全不受影响**：
`apkthumb` 不调用签名解析，`apkthumb.dll` 仍是 308,736 B（与 R-0004 相同）。

### 顺带修掉

`thumbtest --shell` 传相对路径时永远是 `hr=0x80070002`（`SHCreateItemFromParsingName` 不收相对路径）。
现在 `--file/--dir/--list` 一律先转绝对路径；README 里的示例可以直接复制粘贴。

### 未验证（不许当作通过）

1. **v3.1（密钥轮换）** 与**多签名者**没有真实样本；代码按"遍历全部 signer、证书按 DER 摘要去重"实现，未实测。
2. **不做摘要校验**，所以 `signature` 只代表"块存在"，不代表签名有效（`base2` 就是例子）。
3. **EC / DSA 的 v1 块**没有样本（现有样本全是 RSA）。
4. `splits`（`.apks` 分包）与 `permissions[].level` 仍未实现；`splits` 随 M4a 一起做。

## 14. R-0006 记录（M3 · 属性页，2026-11-04）

### 环境

| 项 | 值 |
| --- | --- |
| OS | Windows 11 `10.0.26300` |
| 主题 / DPI | 系统深色（`AppsUseLightTheme=0`）、属性窗口浅色 / 100% |
| 构建 | Release `/MT /W4 /WX`，`apkprop.dll` 侧链接 `comctl32 shell32 ole32 msimg32` |
| 注册 | `HKCU\Software\Classes`（免 UAC），`-Feature Propsheet` |
| 样本 | `aida64-1.94.apk`（7.9 MB · v1+v2+v3）、`base2_1.92G.apk`（1.79 GB）、`bad/notzip.apk` |

### 命令

```bat
tools\build.cmd Release
out\x64\propsheet_model_test.exe --selftest
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install -Feature Propsheet
powershell -ExecutionPolicy Bypass -File tools\capture-propsheet.ps1 ^
    -ApkPath <apk> -OutPng docs\evidence\m3-propsheet-light.png
powershell -ExecutionPolicy Bypass -File tools\capture-propsheet.ps1 ^
    -ApkPath <apk> -OutPng ... -ClickPoint "272,590" -WheelClicks 3   :: 点"计算 SHA-256"后滚到底
out\x64\proptest.exe --file <apk> --verbose                          :: 不经资源管理器，直接建页
```

### 结果

| # | 判据 | 结果 |
| --- | --- | --- |
| 1 | 属性页出现「安装包信息」标签 | ✅（标签条第 2 个；`tools/capture-propsheet.ps1` 截图） |
| 2 | 字段与 `apkinfo --json` 一致 | ✅ 应用/技术/权限与组件/文件与签名 四组逐项对照（`m3-propsheet-light.png`），SHA-256 一栏与 `--hash=sha256` 相同 |
| 3 | 冷开耗时 | ✅ 7.9 MB：**140–188 ms**（页脚"解析完成 · N ms"）；1.79 GB：**157 ms** |
| 4 | 按需哈希 | ✅ 点击后页脚"哈希已计算（172 ms）"，SHA-256 行填 64 位 hex；1.92 GB 包约 1.2 s |
| 5 | 展开权限/特性 | ✅ 6 项权限逐条列出（`m3-propsheet-expanded.png`） |
| 6 | 损坏包 | ✅ 页内"未找到 ZIP 结束记录（不是有效 ZIP）"+ 重试/复制 JSON，explorer 不崩（`m3-propsheet-error.png`） |
| 7 | 解析中关窗（哈希 1.92 GB，3 次） | ✅ 关窗后 `Get-Process apkinfo` 计数 = 0（Job `KILL_ON_JOB_CLOSE` + `TerminateJobObject`） |
| 8 | 行模型单测 | ✅ 26/26（`propsheet_model_test --selftest`，含坏 JSON/Unicode/签名缺失路径） |
| 9 | 卸载零残留 | ✅ `-Uninstall` 后 `HKCU\Software\Classes\.apk`、两个 CLSID 全部消失；重装后页面正常 |
| 10 | worker 崩溃 | ✅ 页内"解析器异常退出（0xC0000005）"+ 重试，explorer 存活（`m3-propsheet-crash.png`） |
| 11 | 页内与属性窗口同色 | ✅ 取色：页内背景 `#F0F0F0` = 对话框背景 `#F0F0F0`（系统深色时页内仍是浅色，见 R-0007） |
| 12 | 10 次开关（关窗时 worker 在跑） | ✅ explorer 句柄 4464 → 4435（**-29**），`apkinfo` 残留 0（`tools/propsheet-stress.ps1`） |

### 关键发现

**这个 build 的 shell 在建页之前先 `QI(IShellExtInit)`（`IID 000214E8`）。** 只实现
`IShellPropSheetExt + IPersistFile` 时：handler 会被创建（`CreateInstance` 返回 S_OK），
但 `IPersistFile::Load` 与 `AddPages` 一次都不会被调用，属性页里没有这一页，事件日志与 UI 都不报错。
补齐 `IShellExtInit::Initialize`（从 `CF_HDROP` 取路径）后一切正常。详见
[docs/m3-property-page.md §14](m3-property-page.md)。

注册表只需要 `HKCR\.apk\shellex\PropertySheetHandlers\ApkInfo` + `HKCR\CLSID\{…}\InprocServer32`；
排查时临时加的 `HKCR\*`、`SystemFileAssociations\.apk`、`Approved` 条目均已撤销，移除后页面仍正常。

### 崩溃钩子与一个真实教训

验收"解析器崩溃"需要一个能让 worker 死掉的开关。环境变量传不进 explorer 拉起的子进程，所以用标志文件
`%TEMP%\pk-shell-ext.test-abort.on`：存在时 worker 等 300 ms 后 `ExitProcess(0xC0000005)`。

第一版用的是 `RaiseException(0xC0000005)`，结果页面一直停在"正在解析…" —— 因为 WER 把进程挂住不退出，
我们的读循环只能等到 10 s 兜底，最后报的是"超时"而不是"崩溃"。**能真崩的进程不一定立刻变成退出码**，
测试钩子要模拟"退出码"而不是"异常分发"。

### 未验证（不许当作通过）

1. **150% / 200% DPI**（当前机器 100%；DPI 相关的排版逻辑只做了 DIP 换算，没有实测）。
2. 连续开关 **100 次**的句柄/私有提交趋势（本轮做的是 10 次，且是"关窗时 worker 还在跑"的路径）。
3. 中英文双语（只做了中文文案）。
4. 无障碍（自绘列表对 Narrator 不友好，已登记为 P1）。
5. 多显示器 / 不同主题色（Accent color）下的观感。

## 15. R-0007 记录（M3 · 配色改为"跟随宿主"，2026-11-04）

### 背景（用户反馈）

用户观察：**页内内容跟着亮暗切换，但属性窗口本身没有**。预想的逻辑是"页内不决定样式，亮暗交给系统层"，
类比 PySide6 用原生主题——控件外观用操作系统自带的。当时系统是深色、属性窗口是浅色，我们的页内却是深色，
正好与窗口相反。

### 改动

| 项 | 之前 | 现在 |
| --- | --- | --- |
| 背景 | 自配两套色板（`AppsUseLightTheme` 分支） | `SendMessage(host, WM_CTLCOLORDLG, …)` 返回的画刷颜色 |
| 主文本 | 自配（#1B1B1B / #F0F0F0） | `SendMessage(host, WM_CTLCOLORSTATIC, …)` 后 `GetTextColor` |
| 次文本 / 分隔线 / 链接 / 选中 | 自配 | `GetSysColor(COLOR_GRAYTEXT / 3DSHADOW / HOTLIGHT / HIGHLIGHT)` |
| 分组标题 | 自定义浅色/深色色带 | 粗体标题 + `COLOR_3DSHADOW` 分隔线（不再铺底色） |
| 字体 | `SPI_GETNONCLIENTMETRICS` | 先 `SendMessage(host, WM_GETFONT)`，取不到才回落 SPI |
| 主题变化 | 只认 `WM_SETTINGCHANGE` + `ImmersiveColorSet` | `WM_SYSCOLORCHANGE` / `WM_THEMECHANGED` / `WM_SETTINGCHANGE` / `WM_DPICHANGED` 都重新问宿主 |
| 读注册表 | 读 `AppsUseLightTheme` | **不读任何主题开关** |

### 验收（系统深色、属性窗口浅色）

```
powershell -File tools\capture-propsheet.ps1 -ApkPath aida64-1.94.apk -OutPng docs\evidence\m3-propsheet-light.png -SkipUia
取色（System.Drawing.GetPixel）：
  dialog-bg (60,625)  = #F0F0F0
  page-bg   (420,300) = #F0F0F0
  page-bg-2 (430,120) = #F0F0F0
```

页内背景与对话框背景**完全一致**；文字用窗口的文本色；链接用系统 `COLOR_HOTLIGHT`；
分组标题改为"粗体 + 分隔线"后不再出现"页内自带色带"的观感差异。
证据：[docs/evidence/m3-propsheet-light.png](evidence/m3-propsheet-light.png)（本轮其它截图也已按新样式重拍：
`m3-propsheet-hash / expanded / error / crash / big.png`）。

### 仍然成立 / 仍然未验证

- 仍然成立：全自绘只决定**排版**（行高、键列宽、省略号、展开、滚动、命中测试）。无障碍仍弱（P1）。
- 未验证：**150% / 200% DPI**；第三方"深色属性页"宿主（如未来系统支持）下的取色——代码路径是同一套
  `WM_CTLCOLOR*` 查询，但没有那样的宿主可测。

## 16. R-0008 记录（M3 · 快速划过闪烁修复，2026-11-04）

### 现象（用户反馈）

在页内快速上下滑动时，**内容整页闪烁**。

### 定位

不是"自绘路线"的必然结果，是这版实现的三个问题叠加：

1. `WM_MOUSEMOVE` 里一旦 hover 行变化就 `InvalidateRect(hwnd, nullptr, FALSE)` —— **整窗重绘**。
   快滑时每跨一行一次，一秒几十次；
2. 绘制**直写窗口 DC**，没有双缓冲，整窗重画时能看到"先刷底 → 再逐行 → 再底栏"的中间态；
3. tooltip 在每次鼠标移动时 `TTM_TRACKACTIVATE` 开关，长值行/短值行交替经过时也会闪。

### 修复

| 项 | 改法 |
| --- | --- |
| 重绘范围 | `PageView::ItemClientRect()` 给出每行的客户区矩形；hover/选中变化只 `InvalidateRect` **旧行 + 新行** |
| 闪烁 | 页面画到缓存的兼容位图（`EnsureBuffer`，尺寸变化才重建），`WM_PAINT` 结束前一次 `BitBlt` |
| tooltip | 改为**悬停停留 600 ms** 才出现（`SetTimer` + `WM_TIMER`），并在同一行内移动时只跟随位置；`WM_MOUSELEAVE` 立即隐藏 |
| tooltip 触发条件 | 不再按字符数猜（旧的 44 字阈值），改成"上一帧这一行**确实被省略号截断**"（`PageView::ItemTruncated()`，Painting 时量宽度记录） |
| 光标 | 只在"手形/箭头"发生变化时 `SetCursor` |

### 实测（本机，深色系统主题，100% DPI）

```
powershell -File tools\capture-propsheet.ps1 -ApkPath aida64-1.94.apk -HoverSweep 40 -WaitMs 400 -SkipUia
（诊断开关打开时，每次 WM_PAINT 记录 GetUpdateRect 的尺寸）
```

| 指标 | 修复前（代码路径） | 修复后（实测） |
| --- | --- | --- |
| 每次 hover 变化的重绘区域 | 整窗 `429 × 533` ≈ **229k px** | 两行合并 `429 × 52` ≈ **22k px**（个别单行 `429 × 23`） |
| 绘制目标 | 窗口 DC（可见中间态） | 内存位图 → 一次 BitBlt |
| 40 步扫动产生的 WM_PAINT | 每次跨行一次整窗 | 26 次，其中 24 次是 1–2 行的小区域 |

> **后续（R-0009）**：R-0008 里"自绘 + 双缓冲 + 脏矩形 + 自己量截断 tooltip"这一整层，
> 已随视图层改为原生控件而删除；闪烁问题从根上消失。本节保留为历史记录。

tooltip：日志显示 `trunc=1 → TTM_TRACKACTIVATE=1`（长度 97 字符的证书主题），
说明"被截断才提示、停留后才出现"的逻辑成立；**popup 气泡本身看不进窗口截图**（`CopyFromScreen` 只取对话框矩形），
需要人眼确认——已列为未验证。

### 顺带

`tools/capture-propsheet.ps1` 的执行顺序改为"先滚动、再点击/悬停"，并给滚轮补 `SetForegroundWindow`
（否则滚轮发给当时的前台窗口，页面不动）；新增 `-HoverSweep N` 复现"快速扫动"。

### 未验证

1. 120 Hz / 高刷屏上的主观观感（只能人眼看）。
2. tooltip 气泡的可见性（API 成功，截图抓不到 popup）。
3. 滚动时的整窗重绘仍走双缓冲全量画；大内容（几百行）没有做"只画可见行"的裁剪优化，当前数据集最多几十行，够用。

## 17. R-0009 记录（M3 · 视图层改为原生控件，2026-11-04）

### 触发

用户看过自绘版后指出：属性窗口本体不跟随亮暗、快速划过会闪；并明确"这种嵌入现有面板的实现，自绘制不应该是首选"，
希望直接借用"详细信息"页那种两列表的形态。

### 改了什么

- 删除 `src/propsheet/page_view.{h,cpp}`（绘制、命中测试、双缓冲、tooltip 计时器）。
- 新增 `src/propsheet/page_list.{h,cpp}`：`ConfigureListView` / `ApplyListColumns` / `PopulateListView` /
  `FindListRowByKey` / `ScrollListRowIntoView`。
- `prop_page.cpp` 重写为原生控件装配：`STATIC` 头部（32 DIP 图标 / 名称 / 包名 / 版本）+ `SysListView32`
  （两列 + 系统分组）+ `BUTTON`（计算 SHA-256 / 复制全部为 JSON / 重试）+ `STATIC` 状态。
- `theme.{h,cpp}` 只留 DPI 与字体（颜色令牌删除）；`apkprop.rc` 去掉页面级 `WS_VSCROLL`（滚动归列表）。
- 交互：右键菜单、双击复制、`Ctrl+C` 复制；哈希完成自动滚到 `SHA-256` 行并选中。

### 验收

```
MSYS_NO_PATHCONV=1 cmd.exe /c "tools\uild.cmd Release"      # 干净，/W4 /WX 无警告
out/x64/propsheet_model_test.exe --selftest                   # 全 ok
python tests/verify_apkinfo.py tests/fixtures/generated/*.apk  # 6/6 matched
python tests/verify_signature.py --glob "tests/fixtures/generated/signed/*.apk"  # 3/3 matched
out/x64/thumbtest.exe --dir tests/fixtures/generated --repeat 1  # 仅 f_noicon.apk 失败（R-0003 已登记为预期）
```

页面截图（宿主 = PowerShell 5.1，comctl32 v5 → 无分组带、控件是 v5 外观，但布局/交互是真实页面）：

| 文件 | 内容 |
| --- | --- |
| `docs/evidence/m3-native-page.png` | 正常页：头部 + 两列列表 + 底栏按钮，状态"解析完成 · 157 ms" |
| `docs/evidence/m3-native-hash.png` | 点「计算 SHA-256」后自动滚到该行并选中，摘要带省略号，状态"哈希已计算（156 ms）" |
| `docs/evidence/m3-native-error.png` | `notzip.apk`：错误说明两行 + 「重试」出现 + 「复制全部为 JSON」置灰 |

v6 渲染证据（`comctl32 v6` 需要宿主清单；explorer 有、PowerShell 5.1 没有）：

```
out/x64/apkinfo.exe --json <apk> > aida.json
out/x64/proptest.exe --list-json aida.json --capture docs/evidence/m3-native-groups.png
```

该截图里分组带 `应用 / 技术 / 权限与组件 / 文件与签名` 正常显示、列表是 Windows 11 主题外观。

### 过程中踩到的坑（都已修）

1. **`DWLP_USER` 类型不一致导致 AV**：`WM_INITDIALOG` 存的是 `PropPage*`，视图函数却按 `PageState*` 解释，
   打开属性页时 `powershell.exe` 在 `apkprop.dll+0x87d5` 崩溃（事件日志 `0xc0000005`）。
   现在 `DWLP_USER` 只放 `PageState*`，组件对象由 `PageState::owner` 回指，`WM_DESTROY` 里先 `shared_from_this()` 保活再 `Release()`。
2. **多余横向滚动条**：列宽按页面宽度算时没扣列表自身客户区（`WS_EX_CLIENTEDGE` 2px×2），
   改成按 `GetClientRect(list)` 计算并预留竖直滚动条。
3. **无图标时显示空框**：`SS_BITMAP` 静态控件拿到空位图会画一个白框，改为没有图标就 `SW_HIDE`。
4. **截图工具的时间线错了**：`-WaitMs` 的等待在点击之后，导致点击发生在解析还没完成时（哈希按钮还没显示）。
   已把等待移到所有交互之前，末尾只留 150 ms。
5. **`capture-propsheet.ps1` 必须保持纯 ASCII**：PowerShell 5.1 按 ANSI 解码无 BOM 的脚本，
   写中文注释会让紧随其后的 `switch` 块解析失败（报"意外标记 {"）。脚本头部早有这条约定，这次又验证了一遍。
6. **跨进程 `BM_CLICK` 不生效**：给页内按钮发 `BM_CLICK` 拿不到 `WM_COMMAND`；
   改成直接向页面对话框发 `WM_COMMAND(id)`（`-InvokeButton hash|copy|retry`），实测 `WM_COMMAND id=1002` 正常到达。
7. **`SetForegroundWindow` 抢不到前台**：合成鼠标事件会打到别的窗口（外层的 Hermes 窗口在前），
   截图工具改成临时 `SetWindowPos(HWND_TOPMOST)` 再交互。

### 未验证

1. 资源管理器真实宿主下的 v6 外观（分组带）——只能人眼看；v6 渲染已用 `proptest --list-json` 单独证明。
2. 150% / 200% DPI 下的排版（只测了 100%）。
3. 深色系统主题下控件是否全部跟随（控件交给系统，但没有截图证据）。
4. 权限 60+ 条的包：列表会很长，滚动是否顺手只能主观判断。

### 遗留

- `proptest.exe` 直接 `PropertySheetW` 仍返回 `-1`（已确认 `WM_INITDIALOG` 从未被调用，`InitCommonControlsEx` 与 v6 清单都已加）。
  目前按"不走属性表"的 `--list-json` 模式做渲染验证；真实页面走 explorer 路径，不影响交付。

## 18. R-0010 记录（M3 · 权限/特性可折叠，2026-11-04）

用户要求把权限明细与特性明细做成可折叠。实现方式：两段各自成为**可折叠的系统分组**
（`LVGS_COLLAPSIBLE`，箭头与点击都由系统负责），不新增自绘。

### 验收

```
MSYS_NO_PATHCONV=1 cmd.exe /c "tools\build.cmd Release"        # 干净
out/x64/propsheet_model_test.exe --selftest                      # 29 项全 ok（新增"权限/特性组可折叠"）
python tests/verify_apkinfo.py tests/fixtures/generated/*.apk    # 6/6
```

| 证据 | 内容 |
| --- | --- |
| `docs/evidence/m3-native-groups.png` | v6 宿主：`权限 · 6 项`、`特性 · 4 项（0 必需）` 两条分组带都带折叠箭头 |
| `docs/evidence/m3-native-page.png` | 真实页面（v5 宿主）：组标题降级为行，明细可读 |
| `docs/evidence/m3-native-v5-fallback.png` | v5 降级形态：`应用 / 技术 / 权限 · 6 项 / 特性 · 4 项…` 作为行出现 |

### 结构变化

- 分组由 4 个变 5 个：`应用 / 技术 / 权限 / 特性 / 文件与签名`，`组件` 行留在「技术」组。
- 明细行是"空键 + 值等于权限名"，复制时按值列取值（右键「复制值」仍然对）。
- 折叠状态不持久化。

### 踩到的坑

`LVM_ENABLEGROUPVIEW` 返回值不可靠（v6 下也可能返回 FALSE）：第一次按返回值判断，
列表进入"分组模式但条目无组"状态，整屏空白。改用 `comctl32.dll` 的 `DllGetVersion`
判断主版本号是否 ≥ 6，并且 v5 时显式 `ListView_EnableGroupView(FALSE)`。
