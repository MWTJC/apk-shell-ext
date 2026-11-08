# 安装、卸载与日常维护

> 这份文档会随安装包一起落到安装目录（`{app}\install.md`），
> 所以它只写"用户要做什么"，不写实现细节。实现与验收见 [m5-delivery.md](m5-delivery.md)、
> [verification.md](verification.md)。

## 1. 安装（推荐：安装器，零 UAC）

```bat
apk-shell-ext-setup-<ver>-<arch>.exe
```

- **先选对架构**：`-x64` 给普通 Intel/AMD 机器，`-arm64` 给骁龙 X 一类的 Arm64 机器
  （设置 → 系统 → 系统信息 → "系统类型" 可确认）。装错架构的包会被安装器直接拒绝，
  不会装出一个"装上了但没反应"的半残状态。
- 默认**只给当前用户装**：文件进 `%LOCALAPPDATA%\Programs\apk-shell-ext`，注册表写
  `HKCU\Software\Classes`，**不会弹 UAC**。
- 想给整台机器装：安装器第一页勾"为所有用户安装"（或命令行 `/ALLUSERS`），
  此时会**弹一次 UAC**，文件进 `%ProgramFiles%\apk-shell-ext`。
- 安装页面里有两项可选能力（默认都勾）：**缩略图**（大图标/超大图标视图显示应用自己的图标）与
  **文件图标替换**（小图标/列表/详细信息视图把通用图标换成 APK 样式的图标）。
  属性页是核心功能，没有开关。
- 如果这台机器上已经有别的程序接管了缩略图或文件图标，安装器会**分别弹一个询问框**，
  默认按钮是"否（保持原样）"——不动别人的设置；想换成本程序就选"是"。
- 静默安装（脚本/批量）：

  ```bat
  apk-shell-ext-setup-<ver>-<arch>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
  apk-shell-ext-setup-<ver>-<arch>.exe /VERYSILENT /ALLUSERS      :: 整机，需要提权
  apk-shell-ext-setup-<ver>-<arch>.exe /VERYSILENT /DIR="D:\Tools\apk-shell-ext"
  apk-shell-ext-setup-<ver>-<arch>.exe /VERYSILENT /TASKS=""      :: 只装属性页，不接管缩略图/图标
  apk-shell-ext-setup-<ver>-<arch>.exe /VERYSILENT /OVERWRITE=all :: 已有别的工具时也强行接管
  ```

  静默模式不会弹窗：遇到冲突**默认保持原样**，要接管必须显式 `OVERWRITE`（`thumb` / `icon` / `all`）。

装完不需要重启系统。若资源管理器还显示旧图标：**重启资源管理器**（任务管理器 → 资源管理器 → 重新启动），
或清一次缩略图缓存（见 §4）。

## 1b. 升级

**直接运行新版安装包，不要先卸载。** 安装器会就地覆盖文件，并把记录追加到同一份卸载日志
（应用列表里始终只有一条 `apk-shell-ext`），卸载时按版本逆序回滚。

- 你选的界面语言（`--lang`）会保留，不会被升级重置。
- 上次安装时对缩略图/图标两项功能的勾选会沿用（想改就在本次安装页面里改，或用 `--features`）。
- **升级不会关闭资源管理器，也不会关掉你打开的窗口。** 安装器把被占用的旧 DLL 改名（`*.old`）让位，
  再写入新文件；**新版本在你下次启动资源管理器后生效**（已经打开的窗口继续用旧代码，不会出错）。
  若完成页出现"文件在资源管理器中加载过"的那句提示，就是这个情况。
- 不需要重启电脑。`*.old` 这类旧副本会在下次安装或卸载时清掉。
- 已生成的缩略图缓存会因解析器版本变化自动失效。

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
安装时选了"保持原样"而没有写入的那些注册值，卸载时也不会被碰（它们本来就不属于本程序）。
卸载前会先跑一次 `apkinfo --features all=off`，把"装完之后又手动打开过"的功能也一并收干净。
**不会**碰 `.apk` 的文件关联（"用什么打开"），那是别的软件的事。

## 2b. 功能开关（装好之后随时可改）

三层能力里，属性页是核心，另外两层可以单独关掉（比如这台机器上已经有别的工具在做同一件事）：

```bat
:: 看当前状态
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features

:: 只关缩略图（大图标视图回落到系统默认图标，属性页照常）
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features thumb=off

:: 只关文件图标替换（小图标/列表视图回到通用图标）
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features icon=off

:: 两个都关（等于只留属性页）/ 两个都开
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features all=off
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features all=on
```

- 只改注册表，不删文件；开回来是瞬间的事，不用重装。
- 关的时候只删"本程序写的那条"，别人的注册值原样不动（会打印出来说明是谁占着）。
- 属性页不能关（`prop=off` 会被拒绝并返回 4）：不想要它请直接卸载。
- 图标缓存要等资源管理器重置一次（多数情况自己就刷了，没刷就重启资源管理器）。

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

**少数包明明有图标，却显示默认图标？**
最常见的原因是包内图标是 **WebP**，而这台机器没装 WebP 图像编解码器（Win11 的"Webp 图像扩展"，
Microsoft Store 里可装）。缩略图解码走系统 WIC：没有对应编解码器时该包不出图，退回默认图标，
不影响属性页与其它包。`apkinfo.exe --diag on` 后看日志里的解码错误可以确认。

**小图标分不清 APK / 分包 / HAP？**
三枚图标是同一套样式（一张带折角的纸 + 一条写着字的绿/红色带）：
`.apk` 绿单页（写 APK）、`.xapk/.apks/.apkm` 绿双页（写 XAPK）、`.hap` 红单页（写 HAP）。
16 px 下只有"绿单页 / 绿双页 / 红单页"这三个特征，字母要 48 px 以上才出现。
`DefaultIcon` 指向 `{app}\icons\*.ico`，`apkinfo --status` 能看到实际路径。

**属性页没有"安装包信息"这一页？**
- 属性页只对 `.apk/.xapk/.apks/.apkm/.hap` 出现；
- 若刚换过 DLL，资源管理器里还缓存着旧的——重启资源管理器；
- 打开 `--diag on`，再开一次属性页，看 `apkthumb.log` 里有没有 worker 的记录。

**反过来的情况：随便什么文件（比如 .txt）都有这一页？**
这一页是按**扩展名**注册的，不是挂在"所有文件"（`*`）上——WinRAR 也只对压缩文件出现自己的页。
任意文件都出现，说明注册表里有一条指向本程序的全域残留（调试版本或手工实验留下的），
例如 `HKCU\Software\Classes\*\shellex\PropertySheetHandlers\<名字>` 的值是
`{A7C4E1B0-5E8D-4C3A-9F2B-6D1C8E4A7B02}`。

```bat
"%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --status
:: 末段会写「全域注册：有（具体键）」或「无」
```

处置：把 `--status` 报出的那个键删掉，然后重启资源管理器：

```bat
reg delete "HKCU\Software\Classes\*\shellex\PropertySheetHandlers\ApkInfoTest" /f
taskkill /f /im explorer.exe & start explorer.exe
```

（开发/便携场景用 `tools\register.ps1 -Install` 或 `-Uninstall` 注册时，它也会顺手清掉这类残留——
只删值等于本程序 CLSID 的子键，别人的 handler 不动。）

**`.xapk` / `.hap` 的属性页头部没有应用图标？**
1.0.1 已修（页头图标与缩略图走同一条解析路径）。旧版只看得到 `.apk` 的图标，
因为读图标的退路只认外层 ZIP 根上的 `AndroidManifest.xml`。升级即可，见 §1b。

**我已经装了别的 APK 图标/缩略图工具，会不会冲突？**
不会硬抢：安装时如果探测到 `.apk` 等扩展名上已经有别人的缩略图 handler 或 `DefaultIcon`，
安装器会分别问一次，默认按钮是"否（保持原样）"，选"否"就一个值都不写；
装好之后也可以用 `--features` 单独关掉本程序的那一项（见 §2b）。

**右键 → 属性时偶尔闪过一个"选择一个应用以打开文件"的框？**
那是 Windows 自己：`.apk/.xapk/.hap` 没有默认打开方式，资源管理器在打开属性时会顺带试探一次，
和本扩展无关（本扩展不碰文件关联）。想彻底不出现，只能给这些类型指定一个默认应用。

**升级要不要先卸载？**
不要。直接运行新版安装包即可（见 §1b），应用列表里始终只有一条记录。

**升级时会不会关掉我正在用的资源管理器窗口？**
不会。1.0.2 起安装器不再关闭任何程序（见 §1b）：被占用的旧文件会改名成 `*.old` 让位，
新版本在资源管理器下次启动后生效。安装目录里出现 `apkprop.dll.old` 这类文件属正常，
下次安装或卸载会自动清掉。

**杀软/智能应用控制拦安装器？**
安装器没有签名（v1 不带证书），SmartScreen 会提示"未知发布者"，选"仍要运行"即可。
程序本身不联网、不写自启动、不注入进程；源码与构建脚本都在仓库里，可自行编译核对。

**想彻底回到"装之前"？**
卸载即可（注册表值与安装前完全一致，代码级回滚由 Inno 的卸载器负责）。
残留只可能在 `%LOCALAPPDATA%\apk-shell-ext\`（缓存/日志），删掉这个目录即可。
