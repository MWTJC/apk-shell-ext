# 安装、卸载与日常维护

> 这份文档会随安装包一起落到安装目录（`{app}\install.md`），
> 所以它只写"用户要做什么"，不写实现细节。实现与验收见 [m5-delivery.md](m5-delivery.md)、
> [verification.md](verification.md)。

## 1. 安装（推荐：安装器，零 UAC）

```bat
apk-shell-ext-setup-1.0.0.exe
```

- 默认**只给当前用户装**：文件进 `%LOCALAPPDATA%\Programs\apk-shell-ext`，注册表写
  `HKCU\Software\Classes`，**不会弹 UAC**。
- 想给整台机器装：安装器第一页勾"为所有用户安装"（或命令行 `/ALLUSERS`），
  此时会**弹一次 UAC**，文件进 `%ProgramFiles%\apk-shell-ext`。
- 静默安装（脚本/批量）：

  ```bat
  apk-shell-ext-setup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
  apk-shell-ext-setup-1.0.0.exe /VERYSILENT /ALLUSERS      :: 整机，需要提权
  apk-shell-ext-setup-1.0.0.exe /VERYSILENT /DIR="D:\Tools\apk-shell-ext"
  ```

装完不需要重启系统。若资源管理器还显示旧图标：**重启资源管理器**（任务管理器 → 资源管理器 → 重新启动），
或清一次缩略图缓存（见 §4）。

## 2. 卸载（精确回滚）

三种入口，效果相同：

1. 设置 → 应用 → 已安装的应用 → `apk-shell-ext` → 卸载；
2. 开始菜单/控制面板的程序列表（与上同一入口）；
3. 命令行静默卸载：

   ```bat
   "%LOCALAPPDATA%\Programs\apk-shell-ext\unins000.exe" /VERYSILENT /SUPPRESSMSGBOXES
   "%ProgramFiles%\apk-shell-ext\unins000.exe" /VERYSILENT /ALLUSERS
   ```

卸载会删除：安装目录、两个 COM 类、五个扩展名的缩略图/属性页/`DefaultIcon` 注册值、
应用列表条目、以及自建缩略图缓存（`%LOCALAPPDATA%\apk-shell-ext\thumbs`）。
**不会**碰 `.apk` 的文件关联（"用什么打开"），那是别的软件的事。

## 3. 免安装（便携 / 开发调试）

不想装、或者要测一份新编出来的 DLL：

```bat
:: 注册当前目录里的 DLL（写 HKCU，不需要 UAC），图标用仓库里的 icons\
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Install
powershell -ExecutionPolicy Bypass -File tools\register.ps1 -Uninstall
```

`register.ps1` 支持 `-Feature Thumb|Propsheet|Icons` 只装一部分、`-Extensions apk`
只注册一个扩展名、`-DllDir/-IconDir` 指向别处、`-Scope LocalMachine` 走整机（需管理员）。

## 4. 查状态、清缓存、开诊断

安装目录里的 `apkinfo.exe` 同时是个小工具箱（不用装别的东西）：

```bat
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --status      :: 五个扩展名各自的注册状态
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --cache-info  :: 缓存文件数与体积
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --clear-cache :: 清空自建缩略图缓存
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag on     :: 打开诊断日志
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag off    :: 关掉
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag status :: 开关状态 + 日志大小
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --diag clear  :: 清空日志
```

诊断日志位置：`%LOCALAPPDATA%\apk-shell-ext\apkthumb.log`（开关是 `%TEMP%\apk-shell-ext.debug.on`）。
开关对已经跑着的 explorer 无效，改完重启一次资源管理器。

## 5. 常见问题

**装了但大图标还是默认图标？**
1. 先确认是"大图标/超大图标/内容"视图——小图标/列表/详细信息视图按设计只显示格式图标；
2. `apkinfo.exe --status` 看 `thumb=` 是否"用户/整机"；
3. 重启资源管理器；还不行就 `--clear-cache` 再重启。

**小图标分不清 APK / 分包 / HAP？**
三枚图标分别是绿（APK）、靛（XAPK/APKS/APKM，两叠卡片）、红（HAP，六边形）。
`DefaultIcon` 指向 `{app}\icons\*.ico`，`apkinfo --status` 能看到实际路径。

**属性页没有"安装包信息"这一页？**
- 属性页只对 `.apk/.xapk/.apks/.apkm/.hap` 出现；
- 若刚换过 DLL，资源管理器里还缓存着旧的——重启资源管理器；
- 打开 `--diag on`，再开一次属性页，看 `apkthumb.log` 里有没有 worker 的记录。

**杀软/智能应用控制拦安装器？**
安装器没有签名（v1 不带证书），SmartScreen 会提示"未知发布者"，选"仍要运行"即可。
程序本身不联网、不写自启动、不注入进程；源码与构建脚本都在仓库里，可自行编译核对。

**想彻底回到"装之前"？**
卸载即可（注册表值与安装前完全一致，代码级回滚由 Inno 的卸载器负责）。
残留只可能在 `%LOCALAPPDATA%\apk-shell-ext\`（缓存/日志），删掉这个目录即可。
