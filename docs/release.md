# 发布流程（手工，无 CI）

> 本项目目前不使用 GitHub Actions，发布由人工在本地完成，产物手动上传到 Release。
> 为何：等有人看得上这玩意儿了再说（碎碎念）。

## 0. 版本号只有一个来源

`src/version.h`：

```c
#define APKSX_VERSION_MAJOR 1
#define APKSX_VERSION_MINOR 0
#define APKSX_VERSION_PATCH 0
#define APKSX_VERSION_BUILD 0
#define APKSX_VERSION_STRING "1.0.0"      // 必须是 MAJOR.MINOR.PATCH
#define APKSX_VERSION_FULL   "1.0.0.0"    // 必须是 MAJOR.MINOR.PATCH.BUILD
```

派生关系：

| 用到的地方 | 怎么拿到 |
| --- | --- |
| 三个二进制的 VERSIONINFO | 编译时从 `version.h` 取（`src/*/*.rc`） |
| 安装器 `AppVersion` / `DisplayVersion` | `tools\sync-version.ps1` 写进 `installer\apk-shell-ext.iss` |
| 安装包文件名 | `.iss` 的 `OutputBaseFilename=apk-shell-ext-setup-{#AppVersion}` |

改版本就只改 `version.h` 一处，然后：

```bat
powershell -ExecutionPolicy Bypass -File tools\sync-version.ps1 -Check   :: 只看有没有漂
powershell -ExecutionPolicy Bypass -File tools\sync-version.ps1          :: 同步（pack.cmd 会自动做）
```

## 1. 发版检查单

1. **改版本号**：`src/version.h`（三个数字 + `STRING` / `FULL` 一起改）。
2. **构建**：`tools\build.cmd Release --clean`；确认没有警告（编译选项是 `/W4 /WX`，有警告就编不过）。
3. **打包**：`tools\pack.cmd`（内部会先 `sync-version.ps1`）→ `out\dist\apk-shell-ext-setup-<ver>.exe`。
4. **本机 smoke**（约 5 分钟）：
   - 装：双击或 `/VERYSILENT`；控制面板能看到条目、图标与发布者正常；
   - 大图标视图出缩略图；小图标视图是格式图标；右键属性出「安装包信息」且数字对得上 `aapt2 dump badging`；
   - `apkinfo --features` 显示 `thumb/icon/prop` 全开；`--features icon=off` 后图标回落到系统图标，再 `on` 恢复；
   - 卸：设置里卸载，`%LOCALAPPDATA%\Programs\apk-shell-ext` 与注册表值都不留。
5. **自动化验收**：

   ```bat
   powershell -File tools\verify-install.ps1 -Phase snapshot -Path a.txt
   out\dist\apk-shell-ext-setup-<ver>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
   powershell -File tools\verify-install.ps1 -Phase check -State installed
   "%LOCALAPPDATA%\Programs\apk-shell-ext\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
   powershell -File tools\verify-install.ps1 -Phase check -State removed
   powershell -File tools\verify-install.ps1 -Phase snapshot -Path b.txt
   powershell -File tools\verify-install.ps1 -Phase diff -A a.txt -B b.txt
   ```

   最后一行必须输出 `identical (zero residue)`。
6. **换一台没装过的机器/用户再装一次**（可选但推荐，尤其是改过 `.iss` 的版本）。
7. **记录**：在 [verification.md](verification.md) 追加一条记录；有行为变化就同步
   [install.md](install.md) / [architecture.md](architecture.md) / ADR。
8. **提交**：`git add -A && git commit`，然后打 tag：`git tag -a v1.0.0 -m "v1.0.0" && git push --tags`。
9. **发 Release**（下面两种方式，任选）。

## 2. 上传 Release

先算校验和（写进 Release 说明）：

```bat
powershell -NoProfile -Command "Get-FileHash out\dist\apk-shell-ext-setup-1.0.0.exe -Algorithm SHA256 | Format-List"
```

**方式 A：网页**——GitHub → Releases → *Draft a new release* → 选 tag → 标题 `v1.0.0` →
粘贴下面的模板 → 拖入 `apk-shell-ext-setup-1.0.0.exe` → Publish。

**方式 B：gh CLI**

```bat
gh release create v1.0.0 out\dist\apk-shell-ext-setup-1.0.0.exe ^
   --title "v1.0.0" --notes-file notes.md
```

上传清单（**只传这一个文件**）：

| 传 | 不传 |
| --- | --- |
| `apk-shell-ext-setup-<ver>.exe` | `out\x64\*.dll/exe`（未打包的二进制） |
| （可选）`SHA256SUMS.txt` | 测试夹具 `tests\fixtures\**` |
| | 日志、`build\`、`out\` 里的其它东西 |

## 3. Release notes 模板

```markdown
Windows 资源管理器扩展：让 .apk / .xapk / .hap 显示应用自己的图标，并在属性里多一页安装包信息。

## 安装
下载下面的 `apk-shell-ext-setup-x.y.z.exe`，双击即可（默认按当前用户安装，不弹 UAC；
勾"为所有用户安装"才需要管理员权限）。要求 Windows 10 1809+ / Windows 11，64 位。

## 本版变化
- …

## 已知问题
- 安装包没有代码签名，SmartScreen 会提示"未知发布者"，点"更多信息 → 仍要运行"。
- `.apk/.xapk/.hap` 没有默认打开方式，右键属性时偶尔会闪过系统的"选择一个应用以打开文件"，
  与本扩展无关。

SHA-256（apk-shell-ext-setup-x.y.z.exe）：
`<粘贴校验和>`
```

## 4. 仓库地址

README 里的下载链接写的是 `https://github.com/mwtjc/apk-shell-ext/releases`。
如果仓库最终不在这个地址，改 README 顶部那一处链接即可（没有别处引用）。

## 5. 以后什么时候值得上 CI

- 需要**代码签名**（证书在云端，构建机不应存私钥）；
- 需要**多机/多版本 Windows** 的回归（现在只有一台真机跑验收）；
- 发布频率高到"每次手工打包"成负担。

在那之前，`tools\build.cmd` + `tools\pack.cmd` + 本文的检查单就够了。
