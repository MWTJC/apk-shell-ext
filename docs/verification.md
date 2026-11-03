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

## 2. V2/V3 · 缩略图耗时（冷/热）

**工具**：`tests\thumbbench.exe`（M1 交付，走 `IShellItemImageFactory::GetImage(SIIGBF_THUMBNAILONLY)`，与 Explorer 同一条路径，含 thumbcache）。

```
thumbbench.exe --dir "D:\apks" --size 256 --repeat 3 --json
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
| R-0002 | 2026-11-03 | M1 | APK 缩略图闭环打通：真实 Explorer 管线（`IShellItemImageFactory` → `dllhost.exe` → `IInitializeWithStream`）能出包内图标；解析正确率与 aapt2 一致；坏包不崩 | 见下 §10 | V1（ProcMon 未安装）、V4（VMMap）、V8、真实 Explorer 窗口目视 |

## 10. R-0002 详细记录（M1，2026-11-03）

### 环境

| 项 | 值 |
| --- | --- |
| OS | Windows 11（宿主报告 10.0.26300），x64 |
| 工具链 | MSVC 19.51.36260（VS 18 Community，MSVC 14.51 工具集）· CMake 3.31.3 · Ninja 1.12.1 |
| 构建 | `tools\build.cmd Release`（`/MT`、`/W4 /WX`、`/guard:cf`），产物 `out\x64\{apkthumb.dll, thumbtest.exe}` |
| 样本 | `tests\fixtures\generated\` 4 个 aapt2 真实构建的 APK（PNG/WebP/自适应/无图标）+ `tests\fixtures\generated\bad\` 8 个坏包 + 3 个真实包（aida64 8.2 MB、qwxds 1.1 MB、wwlocation 0.3 MB） |
| 磁盘 | 系统盘 NVMe（`C:`），工作目录 `D:`（同一物理盘） |

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

### 二进制规模（对照 ADR-0004 的"两个 DLL 合计 < 500 KB"）

| 产物 | 大小 |
| --- | --- |
| `apkthumb.dll` | 279 KB |
| `thumbtest.exe`（验收工具，不进交付物） | 379 KB |

### 本阶段未验证（不许当作通过）

1. **V1 零开销**：本机未安装 ProcMon，未能做"详细信息视图滚动 30 s → 0 次 `CreateFile`"的正式判据。
   间接证据：注册表里只有 `.apk\ShellEx\{E357FCCD…}` 一个 handler，没有 `IPropertyStore` / 列 / 预览处理器；
   `thumbtest --file` 的缓存命中路径也不会打开源文件（键来自 `GetFileAttributesEx`）。
2. **V4 explorer 内存**：需要 VMMap/任务管理器采样，未做。
3. **V8 恶意样本**：未取公开的 libwebp 崩溃样本；只验证了"坏 PNG 数据被 WIC 拒绝"。
4. **真实 Explorer 窗口目视**：用 `IShellItemImageFactory`（与 Explorer 同一条管线）验证，
   但没有切到"大图标"视图截图；M1 退出条件里的"人工确认 3 个真实包图标"是通过
   逐张查看生成的 PNG 完成的。
5. 句柄泄漏（开关 100 次）与 24 h 负缓存过期策略未做长跑验证。
