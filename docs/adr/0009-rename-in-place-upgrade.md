# ADR-0009：升级不关闭资源管理器——用"改名替换"换掉 `CloseApplications=force`

- **状态**：已采纳（取代 [ADR-0008](0008-feature-toggles-and-upgrade-policy.md) §4 第 2 条）
- **日期**：2026-11-07（1.0.2）
- **相关**：[install.md §1b](../install.md)、[verification.md R-0020](../verification.md)、`installer/apk-shell-ext.iss`

## 上下文

1.0.0/1.0.1 的 `.iss` 用 `CloseApplications=force` 让 Inno 走 Restart Manager 关掉占用旧 DLL 的进程，
再用 `RestartApplications=no` 收尾（因为历史实测过：RM 关不掉 COM Surrogate，默认 `yes` 会以
"某些应用程序无法关闭"中止静默升级）。

用户在 1.0.1 覆盖安装时报告：安装器提示要关闭 `explorer.exe`，关闭后**约 10 秒没有任何进度**，
之后资源管理器自己回来，安装器才跳到完成页。

实测（本机 Win11 24H2，用同一套 `RmStartSession → RmRegisterResources → RmShutdown(RmForceShutdown)`）：

| 场景 | `RmShutdown(force)` 耗时 | 资源管理器 |
| --- | --- | --- |
| 2 个 explorer 进程 | **30.5 s** | 8 ms 后仍在（另一个进程没被登记） |
| 1 个 explorer 进程 | 0.38 s | **60 s 内没有回来**，需手工 `Start-Process explorer` |

另外查 Inno 源码（`Projects/Src/Setup.Install.HelperFunc.pas`、`Setup.MainForm.pas`）：

- `RmShutdown` 在 **UI 线程**调用，向导停在 Installing 页、进度条只跟文件字节走——那段时间**没有进度可显示**。
- 只有 `RestartApplications` 为 yes 时 Inno 才调 `RmRestart`；`no` 时"关掉的应用要不要回来"不归安装器管。
- `[Files]` 的 `restartreplace`（排队到重启替换）分支带 `and IsAdmin`，**按用户安装时无效**。

也就是说：这条路径的耗时不可预测（0.4–30 s）、体验不可解释（无进度）、而且把"桌面能不能回来"押在系统行为上。
对一个"只增加右键属性页和缩略图"的扩展来说，这个代价明显过大。

## 决策

**升级不再关闭任何程序。** Windows 允许给一个**已被进程加载**的 DLL 改名（正在运行的进程继续用旧映像，
新进程加载新文件），所以安装器只做一件事：

1. `[Setup]` 改为 `CloseApplications=no`；
2. `[Code]` 在 `ssInstall` 阶段（写文件之前）把 `{app}` 下的 `apkthumb.dll` / `apkprop.dll` / `apkinfo.exe`
   改名成 `*.old`（同名副本已存在时加数字后缀）；
3. `[Files]` 照常写入原名的文件；
4. `ssPostInstall` 尝试删除 `*.old`；删不掉（宿主进程还在用）就留着，**下次安装或卸载时清掉**
   （`[UninstallDelete]` 也登记了这三个通配）；
5. 若安装中途失败/取消（新文件没写成），`DeinitializeSetup` 把 `*.old` 改回原名，避免留下一个坏掉的安装目录；
6. 发生"改名让路"时，完成页多一句提示：新版本在资源管理器下次启动后生效，已打开的窗口不会被关。

## 后果

**正面**

- 升级全程零打断：不关窗口、不重建桌面、不出现无进度等待（实测整段替换 157 ms，见 R-0020）。
- 不再依赖 Restart Manager 能否关掉 `dllhost` / `explorer`，也不再需要 `force` 这个"小心使用"的开关。
- 按用户安装（默认路径）与整机安装走同一条逻辑，不像 `restartreplace` 那样只有管理员才有效。

**负面 / 已接受的限制**

1. **已经加载旧 DLL 的进程继续用旧代码**：本次打开的属性页、本次会话里的缩略图，可能仍是旧版本的行为，
   直到 `explorer.exe` 自然重启（用户视角是"下次开机/重启资源管理器后生效"）。我们选择明确提示，而不是偷偷关掉它。
2. 安装目录里可能短暂出现 `*.old` 文件（最长到下次安装/卸载）；它们不影响加载，也不算残留
   （`verify-install` 的快照会看到，卸载会清）。
3. 若目标文件连改名都失败（权限/被独占锁定），退回 Inno 原有的 `restartreplace`（管理员）或报错——
   这条路没变，只是从"常态"降为"异常"。

## 替代方案与被否原因

| 方案 | 否决原因 |
| --- | --- |
| 保持 `CloseApplications=force` + `RestartApplications=yes` | 只去掉"桌面不回来"的风险；0.4–30 s 的无进度等待仍在，且 `RmRestart` 依赖应用注册过 `RegisterApplicationRestart` |
| `CloseApplications=no` + 只靠 `restartreplace` | 文档与源码都写明非管理员无效，而本产品默认按用户安装；文件被占用时安装直接失败 |
| 升级时先卸载再安装 | 用户明确要"就地覆盖、应用列表只有一条"，且会丢注册表/设置的连续性 |
| 自带 updater 常驻进程 | 与 ADR-0003（不常驻）冲突，为一个安装动作引入常驻进程不值 |

## 修订条件

- 如果将来出现"必须替换正在被加载的文件内容（而不是换文件）"的需求，或 Windows 改变"已加载 DLL 可改名"的行为，
  重新评估本决策。
- 如果实测出现"改名后新进程仍加载到旧映像"（例如文件系统缓存/hardlink 场景），需要在安装后强制一次
  `SHChangeNotify` 并提示重启资源管理器。
