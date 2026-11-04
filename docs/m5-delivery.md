# M5 · 交付打磨（设计与实施记录）

> 目标：[roadmap.md §M5](roadmap.md) 的六件事——安装器、VERSIONINFO + 格式图标、
> 诊断开关、中英文、缓存清理、`IExtractIcon` 评估结论。本文写"做了什么 / 为什么这样做 /
> 实测数字 / 未做什么"。验收原始输出在 [verification.md R-0014](verification.md)。

状态：**2026-11-04 完成**。产物：`out\dist\apk-shell-ext-setup-1.0.0.exe`（2.1 MB，
含 3 个二进制 + 3 枚图标 + `install.md`）+ `out\x64\*.dll/exe`（免安装路线仍可用）。

## 1. 分发：Inno Setup，默认按用户装（零 UAC）

决策与备选方案见 [ADR-0007](adr/0007-inno-setup-per-user-install.md)。要点：

- `installer\apk-shell-ext.iss` 一份脚本描述文件、注册表（5 个扩展名 × 缩略图/属性页/DefaultIcon）、
  卸载删除项；`tools\pack.cmd` 一条命令出单文件安装包。
- 默认 `PrivilegesRequired=lowest` → `%LOCALAPPDATA%\Programs\apk-shell-ext` + `HKCU\Software\Classes`，
  **一次 UAC 都不弹**；安装界面可切到"为所有用户安装"，那一档才要一次 UAC。
- 注册表全部用 `Root: HKA` + `Software\Classes\...`。**踩过的坑**：写 `Root: HKCR` 时
  非管理员模式会去写 HKLM，`RegCreateKeyEx` 返回代码 5，Inno 直接把整个安装回滚
  （`/LOG` 里能看到"创建注册表项时出错…拒绝访问"）。
- 卸载：`unins000.exe`，声明式撤销；额外用 `[UninstallDelete]` 删掉自建缩略图缓存
  与诊断日志（纯派生数据）。

自研安装器的教训（为什么放弃，写下来免得下次重犯）：
journal 用固定 `wchar_t buf[64]` 拼字符串被截断 → 卸载时键名对不上；
正在运行的 `apkinst.exe` 删不掉自己（需要自转发到 `%TEMP%` 并等父进程退出）；
文件占用要重试 + 排重启删除。这三条 Inno 已经处理好了。

## 2. 静态格式图标 + `DefaultIcon`

> 2026-11-04 改版（v2）：原来那套"彩色圆角方块 + 白色图形"（绿机器人 / 靛紫双卡 / 红六边形）
> 换成用户指定的参考样式（参考图不入库，几何常量即事实来源，见 `tools\gen_icons.py` 顶部）——
> **一页纸 + 右上折角 + 横跨纸边的色带 + 白字**；
> `.apk` 与多 APK 容器统一用安卓绿。改版记录见 [verification.md R-0015](verification.md)，
> 对照图 `docs/evidence/icon-v2-sizes.png`、实拍 `docs/evidence/icon-v2-defaulticons-details*.png`。

| 扩展名 | 图标 | 形状 | 颜色 |
| --- | --- | --- | --- |
| `.apk` | `icons\apk.ico` | 单页 + 折角，色带 "APK" | 安卓绿（`#AACC34` / 带 `#5FB74B`） |
| `.xapk` `.apks` `.apkm` | `icons\apks.ico` | 两页错开（一包多 APK），色带 "XAPK" | 同上 |
| `.hap` | `icons\hap.ico` | 单页 + 折角，色带 "HAP" | 红（`#FA8578` / 带 `#C62A45`） |

- 由 `tools\gen_icons.py`（Pillow，`uv run --with pillow python tools\gen_icons.py --preview`）生成并**入库**：
  Explorer 运行时直接读文件，构建不依赖 Python。每枚 ico 内含 16/24/32/48/256 五档，
  **每档按自己的分辨率画**（256 缩到 16 会糊成色块）；48 起才画 "APK"/"XAPK"/"HAP" 字样。
- 字形是 **Nunito Black**（SIL OFL）：字体随仓库走（`tools\fonts\Nunito-Variable.ttf` + `OFL-Nunito.txt`），
  这样换机器/换系统字体版本都不会让图标漂移；`--font` 可覆盖，缺字体时退到 Segoe UI Black 并告警。
  参考图的圆头笔画只有圆体才像，Segoe UI Black 是平的——所以宁可信 vendored 字体。
- 小尺寸取舍：16/24/32 px 下 `.apk` 与 `.apks` 只差"单页 / 两页"的 2 px 台阶（颜色按要求统一成绿），
  形状差异 24 px 起看得清，这是像素数的物理限制，已在 `icon-v2-sizes.png` 里留了对照。
- **不做 `IExtractIcon`**（ADR-0001 不变）。理由见 §6。
- 核对"Explorer 实际拿到的是哪一档"用 `tools\icon_probe.ps1`（`SHGetFileInfo` 取 16/32 px 的 HICON 存 PNG）：
  手工工具，不进构建、不设闸门。

## 3. VERSIONINFO

- `src/version.h` 是唯一来源（`1.0.0.0`），`apkthumb.rc` / `apkprop.rc` / `apkinfo.rc` /
  `apkinst.rc`(已删) 都从它取；四个二进制统一 `FILEVERSION 1.0.0.0`。
- 每个资源里都有 **en-US + zh-CN 两个 StringFileInfo 块**，`FileDescription` 随系统语言变化：
  `APK / HAP package thumbnail provider` ↔ `安装包缩略图提供程序（APK / HAP）`。
- `APKSX_PARSER_VERSION` 由 `m4a` 改为 `m5`：它参与缩略图缓存键，所以**装新版本后旧缓存自动失效**
  （roadmap 里"卸载重装后缩略图仍正常（缓存键含版本号）"就是靠这一条）。

## 4. 装好之后的命令（都在 `apkinfo.exe` 里）

```
apkinfo.exe --status                 五个扩展名各自的注册状态 + 图标路径 + 应用列表条目
apkinfo.exe --diag on|off|status|clear   诊断日志开关与日志大小
apkinfo.exe --clear-cache            清空自建缩略图缓存（走 ThumbCache::ClearAll）
apkinfo.exe --cache-info             缓存文件数/体积/目录
apkinfo.exe --lang zh|en|auto        属性页界面语言（写 HKCU\Software\apk-shell-ext\UiLanguage）
apkinfo.exe --features [thumb|icon|all=on|off]   功能开关（ADR-0008；不带参数 = 查询）
```

诊断日志开关仍是 `%TEMP%\apk-shell-ext.debug.on`（存在即写 `%LOCALAPPDATA%\apk-shell-ext\apkthumb.log`），
`--diag on` 只是把它做成一条命令，不再需要"手动建个文件"。

## 5. 中英文

- `src/core/i18n.{h,cpp}`：`Tr(Str)` 取界面文案（77 条），`TrMsg()` 是解析器错误文案的
  **中→英字典**（约 60 条）。语言解析顺序：`HKCU\...\UiLanguage` → `APKSX_LANG` → 系统 UI 语言
  （只认中文族，其它一律英文）。
- 为什么错误文案用字典而不是改 90 处 `err->set(...)`：解析核心不该关心 UI 语言；
  worker 在**出口处**（stderr/JSON）过一遍 `TrMsg` 就够了，未命中的原文直通。
- 覆盖范围：属性页全部文案（分组、字段、按钮、右键菜单、状态栏、错误态）、
  页面标题、诊断分组；worker 的常见错误文案。**WIC/编码器内部错误不翻译**——它们只进调试日志，
  用户看不到，翻译只会让字典变成维护负担。
- 证据：`docs/evidence/m5-propsheet-zh.png` / `m5-propsheet-en.png`（同一台机器、同一个包，
  仅 `UiLanguage` 不同，重启资源管理器后各截一张）。
- 单元：`propsheet_model_test --selftest` 新增 11 条 i18n 断言（zh/en 取词、字典命中、
  未命中原样返回、英文模型里 `SHA-256` 行仍能按 key 回填），总数 60 → **71 ok / 0 fail**。

## 6. `IExtractIcon` 评估结论：不做

- 现状已经满足需求：小图标/列表/详细信息视图用 `DefaultIcon`（纯静态资源，**零调用**，
  V1 判据不受影响）；大图标以上走 `IThumbnailProvider` 出包内图标。
- 做 `IExtractIcon` 能换来的：把"包内图标"塞进 16/32 px。代价是**每次列表重绘都要调用我们**，
  直接违背 B1/P1（滚动一次就读一堆包）。而且 16 px 下包内图标本来就不可辨识（自适应图标尤其）。
- 结论：不做，ADR-0001 不修订。若将来真要在列表里显示包内图标，先改 ADR-0001 再谈。

## 7. 实测（原始输出见 verification.md R-0014）

| 项 | 数字 |
| --- | --- |
| 安装包 | `apk-shell-ext-setup-1.0.0.exe` 2,181,910 字节（LZMA2，clean 构建后重打的最终包）；解包后 3 二进制 + 3 图标 + 文档 |
| 按用户静默安装 | `/VERYSILENT` 退出码 0，**无 UAC**；安装目录 7 个文件 |
| 安装状态自检 | `verify-install.ps1 -Phase check -State installed` → **ok**（4 个文件 + 5 扩展名 × 3 个键 + 两个 CLSID + 应用列表条目） |
| 卸载 | `unins000.exe /VERYSILENT` 退出码 0；`check -State removed` → **ok**；装前/卸后注册表+文件快照 **diff 一致（零残留）** |
| 图标 | 列表视图 16 px 三族可区分（截图）；`DefaultIcon` 指向 `{app}\icons\*.ico` |
| 缩略图（经 Explorer 管线，缓存清空后） | apk 22.2 ms / xapk 47.1 ms / hap 55.0 ms |
| 属性页 | 中文 219 ms、英文 172 ms 出全页（7.9 MB 包） |
| 回归 | fixtures 6/6、真实 apk 3/3、签名 3/3、容器 2/2、HAP 2/2、model 71 ok |

## 8. 未做 / 未验证（不许当作通过）

1. **安装器界面只有英文**：官方 Inno 发行版不带中文 `.isl`。
2. **`--machine` 整机安装（一次 UAC）没有实跑**：需要点 UAC，自动化场景下会挡住会话；
   `.iss` 里 `PrivilegesRequiredOverridesAllowed` 与 `/ALLUSERS` 路径只做了静态审阅。
3. **代码签名**：v1 不带证书（`design.md §8` 的 OV/EV 仍待定），SmartScreen 会提示未知发布者。
4. **`proptest.exe` 在本机失效**：`PropertySheetW` 立即返回 −1（用 M4 时代的旧 DLL 也一样，
   同一份 exe 在 M3 验收时正常），判断是 Windows 11 26300 预览版对非 shell 调用方建属性表的限制；
   属性页验收因此改用**真实资源管理器对话框**截图（证据更强）。工具本身待修。
5. **杀软/Defender 误报、ProcMon 复跑 V1/V9**：M5 没有改动缩略图代码路径，`DefaultIcon`
   也不加载任何 DLL；实时扫描表现与 M4 相同，但本会话没有重新跑 ProcMon（需要管理员 + 真实交互）。
6. 真实 150%/200% DPI 人眼确认仍按用户 2026-11-04 的决定挂起（布局数学由 20,832 组合覆盖）。
7. 英文文案里 `Components` 行（`9 activities · 0 services · 0 receivers · 0 providers`）在窄窗口
   被省略号截断，靠 tooltip 看全——中文同级文案够短。属于可接受的截断，不打算为它改布局。
