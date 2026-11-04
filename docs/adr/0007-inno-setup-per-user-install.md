# ADR-0007 · 安装/分发用 Inno Setup，按用户零 UAC 为主

- 状态：已采纳（2026-11-04）
- 关联：M5 交付（[roadmap.md §M5](../roadmap.md)）、[install.md](../install.md)、
  [architecture.md §4](../architecture.md)、ADR-0001（只做缩略图 provider）

## 背景

M1–M4 的注册一直靠 `tools\register.ps1`：写 `HKCU\Software\Classes`、带 JSON 备份与回滚。
它能用，但作为"交付物"有三个缺口：

1. **没有卸载入口**：用户装完只记得有个脚本，卸载要找仓库、找那个脚本；
2. **回滚靠我们自己记账**：备份文件一丢（换机、清 `%LOCALAPPDATA%`）就只能按"已知键"兜底清理；
3. **没有文件分发**：DLL 留在 `out\x64`，仓库一挪就断。

M5 一开始的实现是**自己写一个 `apkinst.exe`**（约 1100 行 C++：注册表记账 + 文件复制 +
自转发卸载 + 状态/诊断命令），真机跑通了装→卸→零残留。随后确认用户本机已装
Inno Setup 6，于是重新评估"自己写 vs 用现成安装器"。

## 决策

**安装/卸载改用 Inno Setup 6（`installer\apk-shell-ext.iss` + `tools\pack.cmd`），
自己写的 `apkinst.exe` 删除；装好之后要用的查询/维护命令并入 `apkinfo.exe`。**

具体约定：

| 项 | 决定 | 理由 |
| --- | --- | --- |
| 安装器 | Inno Setup 6（本机 `E:\PROGRAMS\Inno Setup 6`，`pack.cmd` 里可用 `%ISCC%` 覆盖） | 卸载器、文件替换（Restart Manager）、静默参数、应用列表条目、关联变更通知全是现成的；回滚是声明式的（`uninsdeletevalue` / `uninsdeletekeyifempty`），不再需要我们维护 journal |
| 默认安装形态 | **按用户**：`{autopf}` = `%LOCALAPPDATA%\Programs\apk-shell-ext`，注册表 `HKCU\Software\Classes`，**零 UAC** | 缩略图/属性页本来就跑在用户上下文（M1 实测 HKCU 注册即生效），没必要为了装一个用户级功能提权 |
| 整机安装 | 安装界面勾"为所有用户安装"或 `/ALLUSERS`，一次 UAC，`%ProgramFiles%` + `HKLM` | 保留"一次 UAC"那条路径，但不再当作默认 |
| 注册表根 | `[Registry]` 一律 `Root: HKA` + `Subkey: Software\Classes\...` | Inno 官方文档明确"不要用 HKCR，改用 HKA + Software\Classes"。实测用 `HKCR` 时非管理员模式直接写 HKLM 被拒（`RegCreateKeyEx` 代码 5，安装回滚） |
| 运行时命令 | `apkinfo.exe --status / --diag on\|off\|status\|clear / --clear-cache / --cache-info / --lang zh\|en\|auto` | 这些"装好之后还想查一下"的能力不该再单独带一个 exe；`apkinfo.exe` 反正一定装在安装目录里 |
| 界面语言 | 属性页/标签跟随系统；`HKCU\Software\apk-shell-ext\UiLanguage` 可强制 `zh`/`en` | 属性页跑在 `explorer.exe` 里，进程外没有环境变量可用，只能落注册表 |
| 安装器界面语言 | 只有英文 | 官方 Inno 发行版不带中文 `.isl`；第三方翻译不入库（不想为一次性界面引入来源不明的资源文件） |
| 便携/开发 | `tools\register.ps1 -Install` 保留 | 编一份新 DLL 就地注册比重新打包安装器快得多；它同时是 Inno 之外的"逃生舱" |

## 备选方案与否定理由

- **继续自研安装器**：功能能对齐，但我们要自己保证"文件被占用/半途失败/自身被删"这些
  边角，M5 实测里就先后踩到 journal 截断、运行中 exe 删不掉、卸载顺序三个坑。
  这些正是成熟安装器已经解决的问题，继续维护是重复造轮子。
- **WiX/MSIX**：MSIX 需要证书与打包流水线；WiX 需要 .NET 工具链，且对 shell 扩展的
  "按用户注册"没有 Inno 这种 `PrivilegesRequired=lowest` 的直给开关。
- **绿色版（只给 zip + register.ps1）**：保留为可选路径，但默认交付必须有一个
  "双击装、应用列表里能卸"的东西。

## 后果

**好处**：一个 2.1 MB 的 `apk-shell-ext-setup-1.0.0.exe`；卸载走 `unins000.exe`，
注册表/文件回滚由 Inno 按声明撤销（实测装→卸后快照 diff 为 0）；升级不需要先卸载；
`/VERYSILENT` 可直接进脚本。

**代价与约束**：

1. 构建机需要 Inno Setup 6（只影响打包，不影响 `tools\build.cmd`）。
2. Inno 的 AppId 决定卸载条目名：`{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B03}_is1`
   （`tools\verify-install.ps1` 与 `apkinfo --status` 都按这个读）。**改 AppId = 换产品**，
   升级会变成两套并存，不允许随手改。
3. `install.md` 会随安装包落地，属于交付物的一部分：改安装/卸载行为必须同步它。
4. 安装器界面语言只有英文这一点在 [m5-delivery.md](../m5-delivery.md) 的"未实现"里登记。
