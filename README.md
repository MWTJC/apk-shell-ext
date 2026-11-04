# apk-shell-ext

让 Windows 资源管理器**认出你手机安装包长什么样**。

装了它以后：

- **大图标 / 超大图标**视图里，`.apk` / `.xapk` / `.hap` 显示的是**应用自己的图标**，不再是一张白纸；
- **小图标 / 列表 / 详细信息**视图里是三枚专门画的格式图标（APK 绿单页、XAPK 绿双页、HAP 红单页），一眼分得清；
- 右键 → 属性，多出一页 **「安装包信息」**：包名、应用名、版本、SDK、ABI、屏幕密度、语言、权限、签名摘要、SHA-256——全部离线解析，不联网。

> **占位：演示动图**（GIF：在资源管理器里滚动一个全是 apk 的文件夹，大图标逐个出图）
> 把文件放到 `docs/evidence/demo-thumbnails.gif` 后，把这两行换成 `![缩略图](docs/evidence/demo-thumbnails.gif)`

> **占位：属性页截图**（PNG：右键 → 属性 → 安装包信息，中英各一张可选）
> 放到 `docs/evidence/demo-propsheet.png` 后换成 `![属性页](docs/evidence/demo-propsheet.png)`

> **占位：小图标对比图**（PNG：同一文件夹里 apk / xapk / hap 三枚格式图标并排）
> 放到 `docs/evidence/demo-icons.png` 后换成 `![格式图标](docs/evidence/demo-icons.png)`

## 安装

1. 到 [Releases](https://github.com/mwtjc/apk-shell-ext/releases) 下载 `apk-shell-ext-setup-x.y.z.exe`；
2. 双击安装。**默认只给当前用户装，不弹 UAC**；想装给整台机器就在第一页勾"为所有用户安装"（那一步会要一次管理员权限）；
3. 装完不用重启。若资源管理器还显示旧图标，重启一次资源管理器即可。

要求：Windows 10 1809+ / Windows 11，64 位。

## 用起来

- **两层能力可以单独关**。如果这台机器上已经有别的工具在做缩略图或图标，安装时可以只勾你要的那一项；
  属性页是核心功能，始终开启。装完也能随时改：

  ```
  "%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features            :: 看当前状态
  "%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features thumb=off  :: 关缩略图
  "%LOCALAPPDATA%\Programs\apk-shell-ext\apkinfo.exe" --features icon=off   :: 关图标替换
  ```

- 卸载走"设置 → 应用"，装了什么卸什么，不留残渣。
- 静默安装、诊断日志、清缓存、语言切换、常见问题：见随安装包落地的 [用户手册](docs/install.md)。

## 它为什么快

> 图标只在要画图时读几十 KB；元数据只在打开属性页时才解析；**解析永远不发生在 explorer.exe 里**。

浏览目录时，资源管理器对我们**一次文件读取都没有**——性能与没装本扩展时相同（配方与数字见 [docs/verification.md](docs/verification.md)）。

## 支持的格式

| 扩展名 | 说明 |
| --- | --- |
| `.apk` | Android 安装包 |
| `.xapk` `.apks` `.apkm` | 多 APK 容器（只解析其中的 base APK，不整包解压） |
| `.hap` | 鸿蒙安装包 |

不支持 AAB / IPA / APPX，理由见 [ADR-0006](docs/adr/0006-format-scope-cut.md)。

## 从源码构建

`tools\build.cmd Release` 出二进制，`tools\pack.cmd` 出安装包；环境与步骤见 [docs/development.md](docs/development.md)。
需要 Visual Studio 的 C++ 工具链（MSVC + CMake + Ninja）与 Inno Setup 6。

## 许可

MIT，见 [LICENSE](LICENSE)。随仓库的 miniz 也是 MIT（`src/third_party/miniz`），图标字体 Nunito 为 SIL OFL（`tools/fonts/`）。

---

设计、架构、验收记录与架构决策都在 [docs/](docs/README.md)。
