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
