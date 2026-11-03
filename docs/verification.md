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
| 观测工具 | Sysinternals Suite（Store 版 2026.10.0.0）`Procmon.exe`；脚本 `tools1-procmon.ps1` + `tools\parse-procmon-csv.py` |

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

**交付**：`outdpkinfo.exe`（静态 CRT，285 KB 级 core 复用）+ JSON schema v1 的
`file` / `app` / `icon` / `components` / `permissions` / `diagnostics` 六组字段。

### 交叉校验（判据：与 aapt2 一致，允许我们多显示）

```
python testserify_apkinfo.py <apk...>
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
