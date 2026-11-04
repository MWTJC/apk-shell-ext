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

| 编号 | 日期 | 阶段 | 结论 | 证据 | 未验证 |
| --- | --- | --- | --- | --- | --- |
| R-0001 | 2026-11-03 | M0 | 文档基线建立；未做任何代码验证 | 本套文档 | M1 起全部待测 |
| R-0002 | 2026-11-03 | M1 | APK 缩略图闭环：真实 Explorer 大图标视图已出包内图标；V1 零开销（详细信息 0 次 vs 大图标 36 次）；8 个坏包优雅失败 | §10、§1.1 | V8、长跑、30 s 长捕获 |
| R-0003 | 2026-11-03 | M2（部分） | `apkinfo.exe --json` 骨架：7/7 与 aapt2 一致；`--hash=sha256` 与 certutil 一致；退出码 0/2/3 | §11 | signature/features/splits、TD-001 |
| R-0004 | 2026-11-04 | M2 | TD-001 关闭：arsc 改流式跳读，19.7 MB 真表 0.7–1.5 ms；大包 3.8–6.9 ms；修掉 typeIdOffset 越界丢图 | §12 | TD-002（deflate 大 arsc） |
| R-0005 | 2026-11-04 | M2 | 签名块（v1/v2/v3/v3.1）22/22 与 apksigner 一致；`uses-feature` 11/11 与 aapt2 一致 | §13 | permissions[].level |
| R-0006 | 2026-11-04 | M3 | `apkprop.dll` 首版：AddPages 零 IO、Job 管 worker、错误态、按需 SHA-256 | §14 | 150%/200% DPI、100 次开关 |
| R-0007 | 2026-11-04 | M3 | 配色改为跟随宿主属性窗口（不自带亮暗） | §15 | 深色系统主题截图 |
| R-0008 | 2026-11-04 | M3 | 快速划过闪烁修复：双缓冲 + 脏矩形 + tooltip 停留 | §16 | 大内容只画可见行 |
| R-0009 | 2026-11-04 | M3 | 视图层改原生控件（STATIC + SysListView32 + BUTTON），删自绘与颜色令牌 | §17 | 真实宿主 v6 外观、DPI |
| R-0010 | 2026-11-04 | M3 | 权限/特性改成可折叠系统分组（默认收起），v5 宿主降级为行 | §18 | 折叠状态不持久化（有意） |
| R-0011 | 2026-11-04 | M3 补测 + M4a | 布局纯函数 20,832 组合无裁切；100 次开关句柄 9376→9177、残留 worker 0；两份 `.xapk` 与 aapt2 2/2；4 个坏容器优雅失败 | §19 | 真实 DPI 肉眼确认、`.apks/.apkm` 样本 |
| R-0012 | 2026-11-04 | M4d | 鸿蒙 `.hap`：`module.json` + `$media:` 三级图标启发式；2/2 与 `module.json` 一致；3 个坏 HAP | §20 | `$string:`（当时未做，R-0013 已补）、layered 合成 |
| R-0013 | 2026-11-04 | M4d | 应用名解析（RestoolV2 `resources.index`）+ 范围裁剪（ADR-0006）+ 信息页"未实现"口径；`刷题宝`/`LibreTV` 2/2 | §21 | 多语言限定词、Restool v1 老索引、id→文件名、`.hsp`、HAP 签名 |
| R-0014 | 2026-11-04 | M5 | 交付打磨：Inno 安装器（按用户零 UAC）装→卸零残留、三枚格式图标（16–256 五档）+ `DefaultIcon`、VERSIONINFO 1.0.0.0、诊断/缓存/语言命令、中英双语属性页；全套回归 6/6·3/3·3/3·2/2·2/2·71 ok | §22 | `/ALLUSERS` 实跑、代码签名、ProcMon 复跑、`proptest` 修复 |

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
| `docs/evidence/m3-native-collapsed.png` | v6 宿主（默认态）：两条分组带都收起，明细行不占位，页面不用滚动就能看全 |
| `docs/evidence/m3-native-groups.png` | v6 宿主（早期版本，默认展开）：两条分组带都带折叠箭头 |
| `docs/evidence/m3-native-page.png` | 真实页面（v5 宿主）：组标题降级为行，明细可读 |
| `docs/evidence/m3-native-v5-fallback.png` | v5 降级形态：`应用 / 技术 / 权限 · 6 项 / 特性 · 4 项…` 作为行出现 |

### 用户确认（2026-11-04）

资源管理器真实宿主下：箭头可鼠标单击折叠/展开、默认收起、控件跟随系统主题——用户回复"目前没有问题"。
M3 主要验收项到此全部通过。

### 结构变化

- 分组由 4 个变 5 个：`应用 / 技术 / 权限 / 特性 / 文件与签名`，`组件` 行留在「技术」组。
- 明细行是"空键 + 值等于权限名"，复制时按值列取值（右键「复制值」仍然对）。
- 折叠状态不持久化；默认收起（`LVGS_COLLAPSED`），用户点箭头展开只对本次会话有效。

### 踩到的坑

`LVM_ENABLEGROUPVIEW` 返回值不可靠（v6 下也可能返回 FALSE）：第一次按返回值判断，
列表进入"分组模式但条目无组"状态，整屏空白。改用 `comctl32.dll` 的 `DllGetVersion`
判断主版本号是否 ≥ 6，并且 v5 时显式 `ListView_EnableGroupView(FALSE)`。

## 19. R-0011 记录（M3 补测 · DPI 与 100 次开关；M4a · .xapk/.apks/.apkm，2026-11-04）

### 环境

与 §0 基线一致：Windows 11 x64、100% 显示缩放、驱动 MSVC 14.51（VS 18）+ Ninja；
扩展以 HKCU 注册（`.apk/.xapk/.apks/.apkm`）；本记录开始前重启过一次 `explorer.exe`（换新 DLL）。

### 变更（M3 侧：让 DPI 可自动验）

`"150%/200% 下不裁切"` 原本只能人眼在真实缩放下看。这次把布局数学从
`prop_page.cpp::Relayout` 抽成纯函数：

```
PageLayout ComputePageLayout(int dpi, int width, int height, bool show_hash, bool show_retry)
```

（`src/propsheet/page_layout.{h,cpp}`，进 `apksx_pagemodel` 库；`Relayout` 只把算出来的矩形交给 `MoveWindow`）。
`propsheet_model_test --selftest` 增加布局扫描：6 档 DPI（96/120/144/168/192/240，即 100%–250%）×
客户区 360–900 × 400–1000 DIP × 4 种按钮组合 = **20,832 个组合**，每个组合断言 5 条不变量。

### 结果 · 布局 / DPI

```
selftest: page layout / dpi
  (swept 20832 layouts: dpi 96..240 x client 360..900 x 400..1000 DIP x 4 variants)
  ok   所有控件都在客户区内（无裁切）
  ok   没有任何负宽高
  ok   底栏按钮/状态文字互不重叠
  ok   头部文字不压图标
  ok   列表不与头部/底栏重叠
  ok   96 DPI 图标为 32px @ (12,12)
  ok   96 DPI 列表左右各留 12px
  ok   96 DPI 按钮宽度 128 / 112
  ok   96 DPI 底栏高 34px、按钮高 24px
  ok   150%/200% 图标按比例放大到 48px / 64px
  ok   150%/200% 内边距按比例放大
  ok   200% 列表宽度是 100% 的两倍
```

黄金值固定了 100% 的几何（改动布局会先炸这里），等比断言固定了 150%/200% 的缩放关系。

### 结果 · 100 次开关（句柄趋势）

命令：`powershell -File tools/propsheet-stress.ps1 -ApkPath tests\fixtures\real\aida64-1.94.apk -Cycles 100`

```
explorer handles before: 9376
  cycle  25: handles=9934 apkinfo=0
  cycle  50: handles=9395 apkinfo=0
  cycle  75: handles=9260 apkinfo=0
  cycle 100: handles=9177 apkinfo=0
explorer handles after 100 cycles: 9177 (delta -199)
leftover apkinfo processes: 0
```

判据（"explorer 句柄/私有提交无单调增长"）：**通过**。前 20 次有一次性爬升（9376→9934，属性窗初次创建），
30 次之后回落到 9398 并持续下降；结束时比开始时低 199。残留 `apkinfo.exe` 全程为 0。

容器样本同样跑了一遍（`-ApkPath build\m4a-view\Poweramp.xapk -Cycles 20`，20 次都是解析
`.xapk` 的内层 base）：

```
explorer handles before: 2236
  cycle  5: handles=2063 apkinfo=0
  cycle 20: handles=2062 apkinfo=0
explorer handles after 20 cycles: 2062 (delta -174)
leftover apkinfo processes: 0
```

（before 只有 2236 是因为本会话重启过 explorer；20 次之间句柄持平，结束比开始低 174，无泄漏迹象。）

### 结果 · M4a（多 APK 容器）

| 样本 | kind | base | APK 数 | 图标来源 | worker | 缩略图（进程内 / 经 shell） |
| --- | --- | --- | --- | --- | --- | --- |
| Poweramp ...APKPure.xapk（22 MB） | xapk | `com.maxmpz.audioplayer.apk` | 2（base + `config.armeabi_v7a`） | `res/mipmap-xxxhdpi-v9/app_icon_rounded.png` 192×192 | 13.6 ms | 24 ms / 110 ms（冷，含 dllhost） |
| PUBG ...APKPure.xapk（**1.25 GB**） | xapk | `com.tencent.ig.apk` | 1（base 107 MB + 1.1 GB obb） | `res/hW.png` 192×192 | 9.5–12.4 ms | 35 ms / 20 ms（冷/热） |

- `tests/verify_container.py`：**2/2 一致**（package / versionCode / versionName / minSdk / targetSdk 对
  `aapt2 dump badging`；`splits` 对 ZIP 里根目录的 `*.apk` 列表；图标条目在 base 里确实存在）。
- 坏容器（`tests/gen_container_fixtures.py` 造）：4/4 优雅失败——

```
xapk_no_apk.xapk        apkinfo exit=2  「包内既没有 AndroidManifest.xml，也没有内层 .apk」
xapk_broken_base.xapk   apkinfo exit=2  「容器里的 base.apk 打不开（中央目录损坏）」
xapk_bomb_base.xapk     apkinfo exit=2  同上（假炸弹先被 ZIP 结构挡住，没有无限解压）
xapk_truncated.xapk     apkinfo exit=2  「未找到 ZIP 结束记录（不是有效 ZIP）」
```

  `thumbtest --file` 对这四个全部 `ok:false`、耗时 1.7–3.5 ms，进程不崩、不留负缓存之外的副作用。

- Explorer 证据：`docs/evidence/m4a-xapk-explorer.png`（三个文件的大图标视图：`aida64.apk` 出 AIDA64 图标、
  `Poweramp.xapk` 出 Poweramp 图标、`PUBG.xapk` 出 PUBG 图标）。
- 属性页证据：`docs/evidence/m4a-xapk-propsheet.png`（Poweramp 头部 + 应用/技术/权限组，解析完成 282 ms）、
  `docs/evidence/m4a-xapk-propsheet-splits.png`（滚动到底：「分包 · 2 项」= `[base] com.maxmpz.audioplayer.apk（15.1 MB）`、
  `[配置] config.armeabi_v7a.apk（6.0 MB）`；「文件与签名」组首两行 = `内层 APK` 与 `安装包类型 XAPK（2 个 APK）`）。
- 单元：`propsheet_model_test --selftest` 全绿（含新增的 container/splits 断言与布局扫描）。

### 未验证（不许当作通过）

1. **真实 150%/200% 显示缩放下的人眼确认**：本次证明的是布局数学（几何不越界/不重叠/等比），
   字体渲染与真实对话框尺寸仍需要在 150%/200% 缩放下开一次属性页看一眼。
2. `.apks`（bundletool）与 `.apkm`（APKMirror）**没有真实样本**：只有启发式（`toc.pb`/`info.json`）与坏样本。
3. **deflate 的 base.apk**：两个真实样本的 base 都是 Stored（走 `SliceSource`），512 MB 上限那条分支没有真实数据。
4. **容器根 `icon.png` 兜底**：两个样本的 base 里都有 launcher 图标，兜底路径只被代码覆盖，未被样本触发。
5. 嵌套两层以上（zip 套 zip 套 zip）没有测；`formats.md §8` 的限额是 ≤ 2 层。
6. `.hap`（M4d 对象）不在本记录范围内。

## 20. R-0012 记录（M4d · 鸿蒙 .hap，2026-11-04）

### 环境

与 §0 基线一致；扩展以 HKCU 注册（`.apk/.xapk/.apks/.apkm/.hap` 五个扩展名）；
本记录开始前重启过 `explorer.exe`（换新 DLL）。

### 变更

- 新增 `src/core/hap.{h,cpp}`：`module.json`（JSON）解析 + `$media:` 图标三级启发式；
  `LooksLikeHap()` 的判定放在 APK/容器之前（根上有 `module.json`、没有 `AndroidManifest.xml`）。
- `apk_info` 增加 `hap` 分支（`ScanHapZip`）与 `HapInfo`；worker JSON 增加 `hap` 块，`kind` 写 `hap`。
- 属性页：技术组显示 模块 / 主 Ability / API 版本 / 编译 SDK / 设备类型 / Ability 计数，
  文件组显示「安装包类型 HAP（鸿蒙）」「名称引用 `$string:…`」；**签名行改成"未解析（HAP 签名块 v1 不做）"**——
  HAP 的签名不是 APK Signing Block，写"可能未签名"是假信息。
- `register.ps1` 默认扩展名加 `.hap`；新增 `tests/verify_hap.py`；`tests/gen_container_fixtures.py` 增加 3 个坏 HAP。

### 结果 · 真实样本

```
$ python tests/verify_hap.py "tests/fixtures/real/*.hap"
  ShuaTiBao108.hap: com.xm.ShuaTi 1.0.0 (1000000) module=entry
      icon=resources/base/media/foreground.png [png 512x512] 0.48 ms
  entry-default-unsigned.hap: com.libretv.panpp 2.3.6 (1100140) module=entry
      icon=resources/base/media/app_icon.png [png 512x512] 1.95 ms
2/2 HAP matched module.json cross-check
```

交叉校验内容：bundleName / versionName / versionCode / min–targetAPIVersion / module.name / type /
mainElement / deviceTypes / abilities / extensionAbilities / requestPermissions 全部等于 `module.json`；
图标条目在 ZIP 里存在、字节是真 PNG、像素尺寸等于 `icon.width/height`。

| 样本 | kind | 模块 | Ability | 图标来源 | worker | 缩略图（进程内 / 经 shell） |
| --- | --- | --- | --- | --- | --- | --- |
| ShuaTiBao108.hap（408 KB） | hap | entry | 1 + Extension 1 | `foreground.png`（layered 的 foreground 层）512² | 0.49–0.63 ms | 41 ms / 87 ms（冷） |
| entry-default-unsigned.hap（2.2 MB） | hap | entry | 1 | `app_icon.png` 512² | 0.48–1.95 ms | 41 ms / 61 ms（冷） |

- 属性页：HAP 页 **156–172 ms** 出全页（`docs/evidence/m4d-hap-propsheet.png`）。
- Explorer：两个 `.hap` 在"大图标"视图里都显示应用图标（`docs/evidence/m4d-hap-explorer.png`）。
- 坏样本（`tests/gen_container_fixtures.py` 造）：

```
hap_bad_json.hap     apkinfo exit=2  「module.json 不是合法 JSON：字符串未结束（偏移 35）」
hap_no_bundle.hap    apkinfo exit=2  「module.json 里没有 bundleName」
hap_no_icon.hap      apkinfo exit=0  元数据正常，warnings = 未找到可用图标（缩略图 ok:false）
```

  三个的 `thumbtest --file` 全部 `ok:false`、1.7–1.9 ms，进程不崩。

### 未验证（不许当作通过）

1. **`$string:` 标签**：两个样本的应用名都是 `$string:app_name`，页面回落文件名并在「名称引用」行标注。
   解析要读 `resources.index`（RestoolV2 私有格式，头部 `RestoolV2 6.0.0.001`），v1 不做。
2. **layered icon 合成**：只取 foreground 层（刷题宝那张白底红字，缩略图里可读）；没有做前景+背景叠加。
3. **`.hsp` / `.app` 多模块套件**：v1 只认单个 HAP。
4. **`libs/<abi>/*.so`**：两个样本都不带原生库，`abis` 的那段代码没有被真实数据覆盖。
5. **HAP 签名**：鸿蒙的签名块没有解析，页面如实写"未解析"，不是"未签名"。
6. `resources.index` 里的 id→文件名映射（`$media:16777218` 这类）没有实现。

## 21. R-0013 记录（HAP 应用名解析 + 范围裁剪 + "未实现"口径，2026-11-04）

### 变更

1. **范围裁剪**：[ADR-0006](adr/0006-format-scope-cut.md)——v1 只做 `.apk/.xapk/.apks/.apkm/.hap`；
   `.aab` 不做（有实际需求再开 ADR），`.ipa/.appx/.msix` 不做。formats/roadmap/architecture/design/README 已同步。
2. **HAP 应用名**：`src/core/hap.cpp` 增加对 `resources.index`（RestoolV2）的只读解析，
   把 `$string:NAME` 解成默认配置里的 UTF-8 值；来源优先 `mainElement` 那个 Ability 的 `label`，
   再退 `app.label`（鸿蒙桌面显示的是前者）。
3. **信息页口径**：未实现的功能一律直接写"未实现"（HAP 的「签名」行），不再向用户解释解析现状；
   应用名解出来后删掉「名称引用」行，也不再往 warning 里塞"HAP 签名块未解析"这类解释性文案。

### 索引格式的依据（不是猜的）

`resources.index` 的布局取自 restool 自己的读写实现
（`openharmony/developtools_global_resource_tool` 的 `include/resource_table.h` 与 `src/resource_table.cpp`，
`ReadNewFileHeader` / `ReadIdSetHeader` / `ReadResources` / `ReadResourceItem`）：

```
IndexHeaderV2 {version[128], length, keyCount, dataBlockOffset}
KeyConfig     {"KEYS", configId, keyCount, KeyParam[keyCount]}
IdSetHeader   {"IDSS", length, typeCount, idCount}
ResTypeHeader {resType, length, count}
ResIndex      {resId, offset, length, name[length]}
ResInfo@offset{resId, length, valueCount} + valueCount × {configId, dataOffset}
值 @dataOffset: u16 长度 + UTF-8
```

两个实测细节（踩过才知道）：

- 字符串资源的 `resType` 是 **9**，且 **id 的高字节不编码类型**（第一次按 `resId >> 24 == resType` 匹配是错的；
  两个样本都是 `0x0100xxxx` 连续编号）。
- 桌面显示的名字来自 **Ability 的 label**：刷题宝的 `app_name` 是 `ShuaTi`，而 `EntryAbility_label` 才是 `刷题宝`。

### 结果

```
$ python tests/verify_hap.py "tests/fixtures/real/*.hap"
  entry-default-unsigned.hap: com.libretv.panpp 2.3.6 label='LibreTV' (1100140) module=entry
      icon=resources/base/media/app_icon.png [png 512x512] 0.47 ms
  ShuaTiBao108.hap: com.xm.ShuaTi 1.0.0 label='刷题宝' (1000000) module=entry
      icon=resources/base/media/foreground.png [png 512x512] 0.6 ms
2/2 HAP matched module.json cross-check
```

`verify_hap.py` 里的 label 交叉校验是**独立的 Python 解析器**（同一套结构、另写一遍），不是回读我们自己的输出。

属性页（`docs/evidence/m4d-hap-propsheet.png`）：头部 `刷题宝 / com.xm.ShuaTi / 1.0.0 (1000000)`，
应用名 = 刷题宝，技术组 模块 entry（entry）/ 主 Ability EntryAbility / API min 60000020 / 编译 SDK 6.0.0.47 /
设备 phone / 组件 Ability 1 · Extension 1 / 图标 512×512，文件组 `安装包类型 HAP（鸿蒙）`、`签名 未实现`，
**156 ms** 出全页。

回归：`propsheet_model_test --selftest` 50 ok / 0 fail；`apkinfo` 6/6（fixtures）+ 3/3（真实 apk）；
容器 2/2；HAP 2/2；签名 3/3；`thumbtest --dir` 仍是 1 个预期失败（`f_noicon.apk`）。

### 未验证

1. **多语言 / 限定词**：只取"默认配置"（`keyCount == 0` 的 configId）的值；中文/深色等限定配置不参与选择。
   两个样本里 `start_window_background` 有 `cfg0/cfg1` 两条，说明该结构确实会被用到，但我们没有多语言样本可验。
2. **Restool v1 老索引**（版本串 `"Restool "` 而非 `"RestoolV2 "`）：代码只认 v2，v1 包会走"应用名未解析"。
3. `$media:<资源 id>`（layered 里那种）的 id→文件名映射仍没做，只按 JSON 键名推断。
4. `.hsp` / `.app` 多模块套件、HAP 签名块：均未实现（信息页写"未实现"）。

## 22. R-0014 记录（M5 交付打磨，2026-11-04）

### 环境

与 §0 基线一致。本轮的注册走**安装器**（Inno Setup 6，按用户装），不再用 `register.ps1`；
验证过程中 `explorer.exe` 被重启过 3 次（换 DLL / 换图标缓存 / 换界面语言）。

### 变更

1. **分发**：`installer\apk-shell-ext.iss`（Inno Setup 6）+ `tools\pack.cmd`；自研 `apkinst.exe` 删除，
   其"装好之后"的命令并入 `apkinfo.exe`（`--status / --diag / --clear-cache / --cache-info / --lang`）。
   决策见 [ADR-0007](adr/0007-inno-setup-per-user-install.md)。默认按用户安装（零 UAC），
   `{autopf}` = `%LOCALAPPDATA%\Programs\apk-shell-ext`，注册表 `HKCU\Software\Classes`。
2. **格式图标**：`tools\gen_icons.py` 生成 `icons\{apk,apks,hap}.ico`（16/24/32/48/256 各档独立绘制），
   安装器与 `register.ps1` 都写 `DefaultIcon`（`.apk` 绿机器人 / `.xapk/.apks/.apkm` 靛紫双卡 / `.hap` 红六边形）。
3. **VERSIONINFO**：`src/version.h` 单一来源，四个二进制统一 `1.0.0.0`，en-US + zh-CN 双块；
   `APKSX_PARSER_VERSION` 由 `m4a` → `m5`（缓存键随之失效）。
4. **中英文**：`src/core/i18n.{h,cpp}`（`Tr` 77 条界面文案 + `TrMsg` 约 60 条解析器错误字典），
   语言顺序 `HKCU\Software\apk-shell-ext\UiLanguage` → `APKSX_LANG` → 系统 UI 语言；
   页面标题从 rc 的 `STRINGTABLE` 移到 i18n。
5. 新增工具与脚本：`tools\pack.cmd`、`tools\verify-install.ps1`（安装状态/残留快照与 diff）、
   `tools\capture-view.ps1`（用 `Shell.Application` 的 `Document.CurrentViewMode` 切视图并截窗口）、
   `tools\list-window.ps1`；`tools\capture-propsheet.ps1` 增加 `-TabName`（双语后中文标题硬编码不再适用）。

### 结果 · 安装 / 卸载（真装真卸）

```
$ tools\pack.cmd
[pack] compiling installer\apk-shell-ext.iss
[pack] done: apk-shell-ext-setup-1.0.0.exe      -> 2,181,910 字节

$ out\dist\apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
exit=0                     （按用户安装，全程没有 UAC 提示）

$ powershell -File tools\verify-install.ps1 -Phase check -State installed
[check] installed : ok

$ "$LOCALAPPDATA\Programs\apk-shell-ext\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
exit=0

$ powershell -File tools\verify-install.ps1 -Phase check -State removed
[check] removed : ok

$ powershell -File tools\verify-install.ps1 -Phase diff -A 装前.txt -B 卸后.txt
[diff] identical (zero residue)
```

`verify-install.ps1 -State installed` 断言的内容：`apkthumb.dll`/`apkprop.dll`/`apkinfo.exe`/`unins000.exe`
四个文件在位；五个扩展名的 `ShellEx\{E357FCCD…}`、`shellex\PropertySheetHandlers\ApkInfo`、
`DefaultIcon` 三个值都等于安装目录里的实际路径；两个 `CLSID\…\InprocServer32` 指向安装目录；
应用列表条目 `{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}_is1` 存在。
`-State removed` 断言同一批键与目录**全部消失**（含卸载条目），即 V6 判据。

两个实测坑（都写进 ADR-0007 了）：

- `Root: HKCR` 在非管理员模式下会去写 HKLM，`RegCreateKeyEx` 返回代码 5，整个安装回滚
  （`/LOG` 原文：「创建注册表项时出错 … 拒绝访问」）。改用官方推荐的 `Root: HKA` +
  `Subkey: Software\Classes\…` 后一次通过。
- 卸载自己时 `unins000.exe` 正在安装目录里跑：Inno 自己处理得了，自研那条路要靠"复制到
  `%TEMP%` 再等父进程退出"才删得掉，这是放弃自研的直接原因之一。

### 结果 · 图标

```
$ uv run --with pillow python tools/gen_icons.py
wrote icons\apk.ico  (20364 bytes, sizes (16, 24, 32, 48, 256))
wrote icons\apks.ico (19320 bytes, …)
wrote icons\hap.ico  (20649 bytes, …)
```

- `docs/evidence/m5-icons-sizes.png`：每枚图标的五个档位 1:1 + 4× 最近邻放大，确认 16/24 px
  仍有形状（不是糊块）、48 px 起有 "APK"/"XAPK"/"HAP" 字样。
- `docs/evidence/m5-defaulticons-list-zoom.png`：资源管理器**列表视图**实拍（4× 放大），
  5 个包：两个 `.apk` 绿机器人、`.xapk` 靛紫双卡、两个 `.hap` 红六边形，16 px 下颜色即可分辨。
- 换图标后 Explorer 会用旧的图标缓存：删 `%LOCALAPPDATA%\IconCache.db` +
  `%LOCALAPPDATA%\Microsoft\Windows\Explorer\iconcache_*.db` 并重启资源管理器才看到新图
  （已写进 `install.md` 的 FAQ）。

### 结果 · 界面语言

```
$ apkinfo --lang zh  ->  [lang] 已设为 中文；重启资源管理器后属性页生效
$ apkinfo --lang en  ->  [lang] 已设为 English；…
$ apkinfo --lang auto -> [lang] 已恢复为跟随系统（当前：中文）
```

- 中文页：`docs/evidence/m5-propsheet-zh.png`（`安装包信息` / `应用` / `技术` / `权限 · 6 项` /
  `特性 · 4 项（0 必需）` / `解析完成 · 219 ms`）。
- 英文页：`docs/evidence/m5-propsheet-en.png`（`Package info` / `App` / `Technical` /
  `Permissions · 6` / `Features · 4 (0 required)` / `Parsed · 172 ms`）。
- 两张图是同一个包、同一个 DLL，只有 `UiLanguage` 与资源管理器重启的区别。
- 单元：`propsheet_model_test --selftest` → **71 ok / 0 fail**（原 60 条 + 11 条 i18n）。

### 结果 · 回归（M5 不得让前面的里程碑退化）

```
python tests\verify_apkinfo.py tests\fixtures\generated\*.apk        6/6
python tests\verify_apkinfo.py tests\fixtures\real\*.apk             3/3
python tests\verify_signature.py --glob "…signed\*.apk"              3/3
python tests\verify_container.py "tests\fixtures\real\*.xapk"        2/2
python tests\verify_hap.py "tests\fixtures\real\*.hap"               2/2
out\x64\propsheet_model_test.exe --selftest                          71 ok / 0 fail
out\x64\thumbtest.exe --dir tests\fixtures\generated --repeat 1      1 个预期失败（f_noicon.apk）
out\x64\thumbtest.exe --shell --file aida64-1.94.apk                 22.15 ms（缓存清空后二次）
out\x64\thumbtest.exe --shell --file Poweramp….xapk                  47.05 ms
out\x64\thumbtest.exe --shell --file ShuaTiBao108.hap                54.99 ms
```

`apkinfo --status`（安装后）：

```
.apk   thumb=用户 prop=用户
        icon="C:\Users\…\Programs\apk-shell-ext\icons\apk.ico",0
… 五个扩展名同上（xapk/apks/apkm → apks.ico，hap → hap.ico）
应用列表条目：有
```

### 未验证（不许当作通过）

1. **整机安装（`/ALLUSERS`，一次 UAC）没有实跑**：会弹 UAC，自动化里会把会话卡住。
   `.iss` 的 `PrivilegesRequiredOverridesAllowed=dialog commandline` 与 `{autopf}` 分支只做了静态审阅。
2. **安装器界面只有英文**：官方 Inno 发行版不带中文 `.isl`，第三方翻译不入库。
3. **代码签名**：v1 无证书，SmartScreen 会拦"未知发布者"；Defender 全盘扫描未复跑（同 M4）。
4. **ProcMon 复跑 V1/V9 未做**：M5 未改缩略图代码路径，`DefaultIcon` 纯静态不加载 DLL，
   但"仍然是 0 次 CreateFile"只有推理没有测量。
5. **`proptest.exe` 失效**：本机 `PropertySheetW` 立即返回 −1（用 M4 时代的旧 DLL 也一样，
   同一工具在 M3 验收时正常），怀疑是 Windows 11 26300 预览版对非 shell 调用方的限制。
   属性页验收改用真实资源管理器对话框截图。
6. 真实 150% / 200% DPI 人眼确认：仍按用户 2026-11-04 的决定挂起。
7. 英文长文案截断：`Components` 行在窄窗口被省略号截断（tooltip 可看全），中文同级文案不会。

## 23. R-0015 记录（格式图标 v2：换成"纸页 + 色带"样式，2026-11-04）

### 环境

与 §0 基线一致。样本：`aida64-1.94.apk`（8.0 MB）、`ShuaTiBao108.hap`（409 KB）、
`Poweramp+…xapk`（21.6 MB），复制成 `sample.apk / sample.hap / sample.xapk` 放进一个空目录。
本机已按用户安装（`%LOCALAPPDATA%\Programs\apk-shell-ext`）；换图后覆盖安装目录里的 `icons\*.ico`，
按 FAQ 删 `IconCache.db` + `iconcache_*.db` 并重启 `explorer.exe`。

### 变更

1. 以用户给的参考图（512×512，按用户要求**不入库**）为样式依据，三枚 ico 重画：
   **一页纸 + 右上折角 + 横跨纸边的色带 + 白字**。
   `.apk` 与 `.apks/.xapk/.apkm` 统一安卓绿（纸 `#AACC34` / 带 `#5FB74B`）；`.hap` 保留红色家族
   （纸 `#FA8578` / 带 `#C62A45`）以便仍能在列表里分开。几何按 512 px 参考图量出，
   写成 `tools\gen_icons.py` 顶部的常量（`SHEET` / `SHEET_RADIUS` / `FOLD` / `BAND_*` / `CAP_HEIGHT`）。
2. 单页 vs 多页：`.apk` 用折角；容器用"两页错开"。16/24/32 px 固定 2 px 台阶——按 5% 算只有 1 px，
   两页会糊成一张（先按 5% 出过一版，16 px 下与 `.apk` 完全同形）。
3. 字形换 **Nunito Black**（SIL OFL）：`tools\fonts\Nunito-Variable.ttf` + `OFL-Nunito.txt` 入库，
   `--font` 可覆盖，字体缺失时退 Segoe UI Black 并告警。参考图的圆头笔画只有圆体像，
   Segoe UI Black 是平头（对照过 Segoe UI Black / Nunito 900 / Rubik 800 / Baloo2 800，Nunito 900 最接近）。
4. 新增手工工具 `tools\icon_probe.ps1`：用 `SHGetFileInfo` 取 16/32 px 的 HICON 存 PNG，
   核对"Explorer 实际画的到底是哪一档"。不进构建、不设闸门。

### 结果 · 生成与解码

```
$ uv run --with pillow python tools/gen_icons.py --preview
wrote D:\Ai Playground\apk-shell-ext\icons\apk.ico (17226 bytes, sizes (16, 24, 32, 48, 256))
wrote D:\Ai Playground\apk-shell-ext\icons\apks.ico (18948 bytes, sizes (16, 24, 32, 48, 256))
wrote D:\Ai Playground\apk-shell-ext\icons\hap.ico (16306 bytes, sizes (16, 24, 32, 48, 256))
wrote D:\Ai Playground\apk-shell-ext\icons\_preview.png
```

- WIC（`PresentationCore` 的 `BitmapDecoder`，与 shell 同一条解码链）：
  `icons\apk.ico: 5 frames -> 16x16, 24x24, 32x32, 48x48, 256x256`，apks / hap 同。
- **坑**：`System.Drawing.Icon(path, 256, 256)` 只回到 48×48——GDI+ 不认 PNG 压缩的 256 帧，
  用它验收会误判成"没有 256 档"。要验帧必须走 WIC。
- 安装器：`ISCC /Q installer\apk-shell-ext.iss` → exit=0，
  `out\dist\apk-shell-ext-setup-1.0.0.exe` **2,166,594 字节**（旧图版本 2,181,910），
  说明 `SetupIconFile` 也接受这套 ico。

### 结果 · Explorer 真的画了新图

```
$ powershell -File tools\icon_probe.ps1 -Dir <showcase> -Out <probe>
sample.apk -> sample.apk-small.png (16x16)
sample.apk -> sample.apk-large.png (32x32)
sample.hap -> sample.hap-small.png (16x16)
sample.hap -> sample.hap-large.png (32x32)
sample.xapk -> sample.xapk-small.png (16x16)
sample.xapk -> sample.xapk-large.png (32x32)
```

- 取回的 HICON 与生成器同尺寸帧**逐像素比对一致**（`ImageChops.difference` 的 bbox 为空，
  通道最大偏差 ≤3/255，属 HICON→Bitmap 的取整）：即 `DefaultIcon` → 新 ico → 对应档位，整条链通，
  没有回退到通用文件图标。
- 实拍：`docs/evidence/icon-v2-defaulticons-details.png` + `-zoom.png`（详细信息视图，16 px：
  `.apk` 绿单页、`.xapk` 绿双页、`.hap` 红单页；截图用 `PrintWindow(PW_RENDERFULLCONTENT)`，
  窗口被挡在前面也能拍到）。
- 对照表：`docs/evidence/icon-v2-sizes.png`（每档 1:1 + 3–6× 放大）。

### 未验证 / 已知取舍

1. 16 px 下 `.apk` 与 `.apks` 只差 2 px 台阶（颜色按要求统一成绿），密集列表里容易混。
   要不要给容器加独立标记（角标 / 双色带 / 换字母），待定。
2. 大图标 / 超大图标视图仍是**包内 launcher icon 缩略图**（M1 契约），本次没动缩略图链路；
   DefaultIcon 的 256 档只确认"帧在、WIC 能解"，没有逐个宿主逐像素验。
3. 只在本机（Windows 11 26300）看过；150% / 200% DPI 人眼确认仍按用户 2026-11-04 的决定挂起。
4. `.hap` 是否也要改成绿色：本次按"apk 与 apks 同色"的要求保留红色家族，若要三色归一需要再改 `FAMILIES`。

## 24. R-0016 记录（功能开关 + 撞车询问 + 升级策略，2026-11-04）

### 环境

与 §0 基线一致。本机按用户安装（`%LOCALAPPDATA%\Programs\apk-shell-ext`）；
本轮多次装卸 + 重启 `explorer.exe`（换图标缓存 / 看开关效果）。
样本：`aida64-1.94.apk`、`Poweramp+…xapk`、`ShuaTiBao108.hap` 复制到 `show2\` 里看列表视图。

### 变更

1. **两个功能开关**（[ADR-0008](adr/0008-feature-toggles-and-upgrade-policy.md)、[architecture.md §4](architecture.md)）：
   缩略图 = 5× `.ext\ShellEx\{E357FCCD…}`，静态图标 = 5× `.ext\DefaultIcon`；属性页恒开、无开关。
   COM 类注册不设开关（活性 CLSID 本身无副作用，保留它"关→开"不必重装）。
2. **安装期**：`[Tasks]` 的 `thumb` / `icon` + `Check: AllowThumb/AllowIcon`；
   `CurStepChanged(ssInstall)` 里探测目标机器上"别人的 handler"（CLSID 有活的 `InprocServer32` /
   `DefaultIcon` 指向真实文件），按功能各弹一次询问，默认按钮 = **否（保持原样）**；
   静默安装默认同样保持原样，`/OVERWRITE=thumb|icon|all` 才接管。
3. **运行期**：`apkinfo --features`（查询）/ `--features thumb=off,icon=off,all=on`（改）。
   只删"指向我们自己"的值（缩略图比 CLSID；图标认 `\icons\<file>` 且文件存在 → 开发态 `out\x64` 也认得出），
   删完连空壳键一起清；`prop=off` 拒绝并返回 4；改完 `SHChangeNotify(SHCNE_ASSOCCHANGED)`。
4. **卸载兜底**：`[UninstallRun]` 在删文件前跑 `apkinfo --features all=off`。
5. **升级**：`UiLanguage` 加 `createvalueifdoesntexist`（不加的话每次升级都把用户选的 `zh`/`en` 顶回 `auto`）。

### 结果 · 关键机制（先做实验，再写代码）

用一次性探针安装器验证了 Inno 的两条行为，它们直接决定方案能不能成立：

```
探针 1：Check 恒 False 的 [Registry] 条目 → 安装不写；值预置为 foreign
        → 卸载后 foreign 仍在（该条目没进卸载日志，卸载时也不碰）
探针 2：Check 为真 → 写入 ours；再把 Check 变成 False 后卸载
        → 值仍被删（卸载是无条件重放日志，不重新求值 Check）
```

结论：`Check` 只在安装期求值一次，所以"保持原样 = 压根不写"就等于"卸载也不会碰"，
**不需要备份/还原机制**（用户 2026-11-04 明确选这条）。

### 结果 · 静默安装矩阵

```
A) 无冲突（机器上只有我们自己的旧值，外加指向 out\x64 的死值）
   $ apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES
   log: stale DefaultIcon ignored on .xapk: "D:\...\out\x64\apks.ico",0   (×4)
        conflict: no foreign thumbnail handler found
        conflict: no foreign DefaultIcon found
        silent install: OVERWRITE= -> thumb takeover=1 icon takeover=1
   $ apkinfo --features
   [features] thumb=5/5 icon=5/5 prop=5/5（核心功能，不可关）

B) 伪造"别人占着"（.xapk 缩略图 → 活 CLSID 指向 shell32.dll；.apk DefaultIcon → shell32.dll）
   不加 OVERWRITE：
   log: conflict: thumbnails already registered by another program: .xapk {9179448A-…}
        conflict: file icons already set by another program:       .apk  "C:\Windows\System32\shell32.dll",0
        silent install: OVERWRITE= -> thumb takeover=0 icon takeover=0
   注册表：.xapk 缩略图仍 {9179448A-…}，.apk 图标仍 shell32.dll；其余 8 个值是我们的
   $ apkinfo --features  ->  thumb=4/5 icon=4/5 prop=5/5

C) 同一机器加 /OVERWRITE=all：
   log: silent install: OVERWRITE=all -> thumb takeover=1 icon takeover=1
   $ apkinfo --features  ->  thumb=5/5 icon=5/5 prop=5/5
```

### 结果 · 交互弹窗（真点了）

正式安装器带许可页、自动化点不动，所以用一个**共用同一份探测/询问代码**的精简探针安装器
（无许可页/无目录页）实跑：`ssInstall` 时弹出询问框，默认按钮是「否(N)」，
向对话框发 `WM_COMMAND IDNO` 后安装继续且**一个值都没写**：

```
probe3 report=[    .apk   "C:\Windows\System32\shell32.dll",0 ]
probe3 decision: takeover=0
registry conflictprobe3 = (空)     <- Check 为假，条目整条跳过
```

截图：[docs/evidence/feature-conflict-prompt.png](evidence/feature-conflict-prompt.png)
（中文系统上正文是英文、按钮是「是/否」——安装器界面只有英文这条老限制，见 §22 未验证 2）。

### 结果 · 开关真的作用到资源管理器

```
$ apkinfo --features icon=off   -> icon=0/5
   （清图标缓存 + 重启 explorer）docs/evidence/feature-icons-off-details.png：
   三个文件全部回到系统通用文件图标
$ apkinfo --features icon=on    -> icon=5/5
   docs/evidence/feature-icons-on-details.png：绿单页 / 绿双页 / 红单页 三枚自定义图标回来
```

### 结果 · 升级与卸载（含一次踩坑）

```
升级（同一个包再装一次）：
  log: 将追加到现有卸载日志: …\unins000.dat     （unins000.dat 9,911 → 19,134 字节，没有 unins001.dat）
  $ apkinfo --lang zh; 再升级一次 -> UiLanguage 仍是 zh   （加 createvalueifdoesntexist 之前是 auto）

空跑/纯属性页安装：
  $ apk-shell-ext-setup-1.0.0.exe /VERYSILENT /TASKS=""
  log: apk-shell-ext: install done (thumb=0 icon=0)
  $ verify-install.ps1 -Phase check -State installed -Features prop   -> ok
  $ apkinfo --features  ->  thumb=0/5 icon=0/5 prop=5/5

"装了属性页 → 事后用命令打开 → 卸载"（本地第一次跑出来 10 个残留键）：
  $ apkinfo --features all=on
  $ unins000.exe /VERYSILENT
  $ verify-install.ps1 -Phase check -State removed
  [check] removed : 10 problem(s)   … 全是 .ext\DefaultIcon / .ext\ShellEx\{…} 的空壳键
```

**原因**：`--features` 当时只删值不删键；声明式回滚里的 `uninsdeletekeyifempty` 只认识"安装时写过的"条目。
**修法**：`--features` 删除后调用 `DeleteKeyIfEmpty`（等价 `uninsdeletekeyifempty`）。修完重跑：

```
  [check] removed : ok
  [snapshot] 19 lines -> snapB.txt
  [diff] identical (zero residue)
```

### 未验证 / 已知限制

1. **"别的程序占着"是伪造的**：用活 CLSID 指向 `shell32.dll` + `DefaultIcon` 指向 `shell32.dll` 模拟，
   没有真装第三方 APK 图标/缩略图工具做交叉验证。
2. **正式安装器的交互弹窗没有逐步点过**：探针安装器与正式安装器共用同一份 `AskAboutConflicts` /
   `AllowThumb` / `AllowIcon` 代码，但许可页之后的完整点击流程没自动化。
3. `/ALLUSERS` 整机安装仍未实跑（沿用 §22 未验证 1）；本轮所有注册表观察都在 `HKCU`。
4. `/TASKS` 只测了 `""` 与默认两档；`/TASKS="icon"` 这类单项组合没单独跑。
5. **"我们装好之后又被别的软件接管、再卸载我们"** 会把别人的值删掉——用户 2026-11-04 明确不管
   （要堵只能把 15 个值全部代码化，ADR-0008 里留了门）。
6. 版本号仍手写在 `.iss`（`AppVersion=1.0.0`），与 `src/version.h` 未同源；待办。

### 补充 · 升级被 dllhost 挡住（本轮实测出来的坑）

装好并**显示过一次缩略图**之后再跑同一个安装包，静默安装直接失败：

```
$ apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=f5.log
exit=5
log: RestartManager 发现一个使用我们文件之一的应用程序: COM Surrogate
     可以使用 RestartManager 来避免重启吗？是 (0)
     用户取消了安装进程。

（加 /CLOSEAPPLICATIONS 也一样）
log: 使用我们的文件关闭应用程序。
     某些应用程序无法关闭。
     用户取消了安装进程。
```

原因：`apkthumb.dll` 被 COM Surrogate（`dllhost.exe`）加载着，RestartManager 关不掉 surrogate 进程，
Inno 于是在静默模式下把"无法关闭"当成取消。改 `CloseApplications=force` 后：

```
$ ... /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=f7.log
exit=0
log: 使用我们的文件关闭应用程序。 (forced)
```

被强关的只有 explorer / dllhost 两个宿主进程（都不承载用户数据），安装后一切正常
（`verify-install -State installed` ok、`--features` 5/5/5、`install.md` 已更新到新版）。
