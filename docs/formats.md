# 安装包格式矩阵与解析要点

> 目标：把"同一件事在不同格式里叫什么、藏在哪里"写成表，避免实现时逐个踩坑。
> 标注：`[实测]` 本机样本；其余为格式规范/官方文档推断，M2/M4 逐项用真实包验证后再改状态。

## 1. 总表

| 格式 | 容器 | manifest | 资源表 | 图标位置 | 难度 | 阶段 |
| --- | --- | --- | --- | --- | --- | --- |
| `.apk` | ZIP | `AndroidManifest.xml`（**二进制 AXML**） | `resources.arsc`（二进制） | `res/mipmap-*/ic_launcher.*`，adaptive 在 `mipmap-anydpi-v26/*.xml` | 中 | M1/M2 |
| `.apks` / `.xapk` / `.apkm` | ZIP 套 ZIP | 内层 `base.apk` | 同上 | 同上（取 base） | 低（多一层） | M4a ✅ |
| `.aab` | ZIP | `base/manifest/AndroidManifest.xml`（**protobuf**，Bundle 格式） | `base/resources.pb`（protobuf） | `base/res/mipmap-*/…`，索引在 `resources.pb` | 高 | **不做**（[ADR-0006](adr/0006-format-scope-cut.md)） |
| `.ipa` | ZIP | `Payload/*.app/Info.plist`（XML 或 binary plist） | 无 | `Payload/*.app/AppIcon*.png` 或 `Assets.car` | 中高（`.car`） | **不做**（[ADR-0006](adr/0006-format-scope-cut.md)） |
| `.hap` | ZIP | `module.json`（JSON） | `resources.index`（**鸿蒙二进制索引**） | `resources/base/media/*.png`，由 `$media:` 引用 | 中（RestoolV2 索引） | M4d ✅ |
| `.appx` / `.msix` | ZIP | `AppxManifest.xml`（XML） | 无 | `Assets\Square44x44Logo*.png`（含 scale/contrast 限定符） | 低 | **不做**（[ADR-0006](adr/0006-format-scope-cut.md)） |
| `.appxbundle` | ZIP 套 ZIP | 内层 `*_x64.appx` | 无 | 同上 | 低 | **不做**（[ADR-0006](adr/0006-format-scope-cut.md)） |

**关键差异**：HAP 的资源不是 `arsc` 而是鸿蒙自己的 `resources.index`（RestoolV2），必须有独立 reader。
**范围**：v1 只实现 `.apk` 家族与 `.hap`；`.aab/.ipa/.appx/.msix` 由 [ADR-0006](adr/0006-format-scope-cut.md) 明确不做，
下面几节的结构知识保留，供将来真要做时不重新调研。

## 2. APK（M1/M2 主战场）

**读取顺序（缩略图只需 1–3 + 5）**

1. **EOCD → 中央目录**：`zip` 结尾 `PK\x05\x06`，注意 ZIP64（大包）。条目名按 ordinal 索引，避免编码/大小写问题（ApkShellext2-Plus 修的正是这类毒包）。
2. **`AndroidManifest.xml`**：AXML 头 `0x00080003`；取 `<application>` 的 `android:icon`（可能是资源引用 `@0x7f…`，也可能是字面路径）。
3. **`resources.arsc`**：`RES_TABLE_TYPE` → `RES_STRING_POOL_TYPE`（全局字符串池）→ `RES_TABLE_PACKAGE_TYPE`（按 package id，通常是 0x7f）→ 目标 `RES_TABLE_TYPE_TYPE`（每种 density 一组）。
4. **签名**：v2/v3 在 `APK Signing Block`（中央目录前），v1 在 `META-INF/*.RSA`。v1 解析代价高，属性页只做"方案列表 + 证书摘要"，证书用 `CryptQueryObject`/`CertGetNameString`。
5. **图标候选**：优先 `mipmap`，其次 `drawable`；density 优先级 xxxhdpi > xxhdpi > xhdpi > hdpi > mdpi > nodpi > anydpi（anydpi 通常是 adaptive XML）。
6. **adaptive icon**：`<adaptive-icon><background/><foreground/></adaptive-icon>`，两层都是资源引用（可能是颜色或图片）。M2 支持合成（前景居中叠背景），失败回退纯前景。

**split APK 处理（M4a 已落地，见 [verification.md R-0011](verification.md)）**：`.apks/.xapk/.apkm` 外层有
`base.apk` + `split_config.*.apk` + 可选 `icon.png`/`manifest.json`。v1 只读 `base.apk`，不做跨 split 合并。

- **侦查顺序**：外层根上有 `AndroidManifest.xml` → 就是 APK；否则收根上的 `*.apk`（子目录里的
  `Android/obb/...` 之类不参与）。种类按元数据文件判定：`toc.pb` → apks（bundletool）、
  `manifest.json` → xapk（APKPure）、`info.json` → apkm（APKMirror），都没有 → `splits`。
- **base 选择**：① 名字就是 `base.apk`；② xapk 的 `manifest.json` 里 `package_name` + `.apk`；
  ③ 排除 `config.*` / `split_*` 后最大的；④ 全部里最大的。选中的那个在 JSON 里标 `base: true`。
- **base 的取法**：Stored → `SliceSource` 在外层文件上开窗口（1.25 GB 的 xapk 也不读进内存）；
  Deflate → 整块解压，上限 **512 MB**，超限报错而不是硬啃。
- **图标兜底**：base 里没有 launcher 图标时用容器根上的 `icon.png`（`icon.fromContainer: true`）。
- **真实样本（不入库）**：`Poweramp ...APKPure.xapk`（2 个 APK：base + `config.armeabi_v7a`）、
  `PUBG ...APKPure.xapk`（1.25 GB，base 107 MB + 1.1 GB obb，只读 base）。两个包与 `aapt2 dump badging`
  的 package/versionCode/versionName 完全一致（`tests/verify_container.py`）。

## 3. AAB（Android App Bundle）——**不实现**（[ADR-0006](adr/0006-format-scope-cut.md)）

> 保留本节只为记录结构；v1 不读 AAB。用户 2026-11-04 决定：AAB 是 Play 的发布格式、
> 不能直接安装，分发侧已经是 `.xapk/.apkm/.apks`（已支持），磁盘上只剩开发者构建产物。

- 结构：`BundleConfig.pb` + `base/manifest/AndroidManifest.xml` + `base/resources.pb` + `base/res/…` + 各模块目录（`feature1/…`）。
- **manifest 与资源表都是 protobuf**（`Resources.proto` / `Bundle` 的 `XmlNode` 定义），不能用 AXML 解析器。方案：把 `.proto` 编译进 worker（`protobuf-lite` 或 hand-written decoder），M4 先只支持 `base` 模块。
- 图标：manifest 里 `android:icon` 同样是资源引用 → 在 `resources.pb` 里查 ID → 得到 `base/res/mipmap-*/…` 路径 → 从 ZIP 读。
- 属性页可额外显示：模块列表（base/feature）、`delivery` 条件（设备/语言/密度）。

## 4. IPA——**不实现**（[ADR-0006](adr/0006-format-scope-cut.md)）

> 保留本节只为记录结构；v1 不读 IPA（苹果生态，Windows 上只出现在助手/越狱工具目录）。

- `Payload/<Name>.app/Info.plist`：可能是 XML plist，也可能是 **binary plist**（`bplist00`）；两种都要解。
- 图标优先级：`CFBundleIcons` / `CFBundleIcons~ipad` 里声明的文件名 → `AppIcon60x60@2x.png` 等约定名 → `iTunesArtwork@2x` → **`Assets.car`**（编译后的资产目录，需解析 CgBI/asset catalog 格式）。
- `Assets.car` 是**唯一的高清来源**（App Store 包通常把 PNG 删掉只留 car）。M4 先做"约定名 + iTunesArtwork"回退，car 解析单列 M4b（可先只支持 `AppIcon` 组里最大尺寸）。
- 签名：`_CodeSignature/CodeResources`（可只显示"存在/缺失"，不做完整校验）。

## 5. HAP（HarmonyOS）

- 结构：`module.json`（模块元数据，JSON）+ `pack.info` + `resources.index`（二进制）+ `resources/base/media/…` + `.abc`（ArkTS 字节码）。
- `module.json` 里 `app.bundleName` / `module.name` / `versionCode` / `versionName` / `minAPIVersion` / `targetAPIVersion` / `deviceTypes`。

**M4d 实施（2026-11-04，记录 [verification.md R-0012](verification.md)）**

- **判定**：ZIP 根上有 `module.json` 且没有 `AndroidManifest.xml` → HAP（`LooksLikeHap`）。HAP 与 APK 走同一个 `ZipReader`，
  缩略图/worker/属性页共用 `src/core/hap.cpp` 这一份解析。
- **字段**：`app.bundleName` → `app.package`，`versionName/versionCode`，`minAPIVersion/targetAPIVersion` →
  `hap.minAPI/targetAPI`（**不写进 `app.minSdk/targetSdk`**：鸿蒙的 API 号与 Android 的 SDK 不是一回事，混写就是撒谎），
  `module.name/type/mainElement`、`deviceTypes`、`abilities` / `extensionAbilities` 计数、
  `module.requestPermissions[].name` → `permissions`、`libs/<abi>/*.so` → `abis`。
- **图标（`$media:` 启发式，三级）**：
  1. `app.icon` = `$media:NAME` → `resources/base/media/NAME.{png,webp,jpg,jpeg}`；
  2. `NAME.json` 是 layered icon：读 `{"layered-image":{"background":"$media:…","foreground":"$media:…"}}`，
     先前景后背景，**按 JSON 的键名推断文件名**（`foreground.png` / `background.png`）——
     因为鸿蒙在这里写的是**资源 id**（如 `$media:16777218`），id→文件名要解 `resources.index`；
  3. 兜底：第一个 ability 的 `startWindowIcon` → 约定名 `app_icon/icon/AppIcon` → `resources/base/media` 里最大的 PNG。
- **不做（v1）**：`$string:app_name` 标签（要解 `resources.index`，属性页回落文件名并在「名称引用」行如实标注）；
  layered icon 的**合成**（只取 foreground 层，白底图在缩略图里可读）；`.hsp` / `.app` 套件（多模块）。
- **真实样本（不入库）**：`ShuaTiBao108.hap`（`$media:layered_image` → foreground 512²，1.0.0）、
  `entry-default-unsigned.hap`（`$media:app_icon` → app_icon 512²，2.3.6，3 条 ohos 权限，4 种设备类型）；
  两个包与 `module.json` 的交叉校验 2/2 一致（`tests/verify_hap.py`）。

## 6. APPX / MSIX——**不实现**（[ADR-0006](adr/0006-format-scope-cut.md)）

> 保留本节只为记录结构；v1 不读 APPX/MSIX（已安装的商店应用图标由系统提供）。

- `AppxManifest.xml` 是普通 XML（`uap:VisualElements` 的 `Square44x44Logo`）。
- 图标资产带限定符：`Assets\Square44x44Logo.scale-200.png`、`.targetsize-44_altform-unplated.png` 等；选取规则：优先 `targetsize-<n>` 接近 44，其次 `scale-400/200/100`。
- `AppxBundle` 是 ZIP 套多个 `.appx`（按架构），取 `x64` 或第一个。

## 7. 图标选取的统一算法（跨格式）

```
candidates = 各格式 reader 产出的 (资源路径, 密度/尺寸, 是否 adaptive)
按 (尺寸 降序, 密度 降序, 格式偏好 PNG > WebP > JPEG) 排序
取第一个可解码的；解码失败降级下一个
全部失败 → 判定为"无图标"，落负缓存，Explorer 显示类型图标
```

**尺寸上限**：只解码到 ≤ 512×512 的源图（防止 4K 图标拖慢首次缩略图）；需要更大时由属性页请求 worker 出图。

## 8. 安全限额（所有格式共用）

| 限额 | 值 | 目的 |
| --- | --- | --- |
| ZIP 条目数 | ≤ 65535 | 防条目爆炸 |
| 单条目解压 | ≤ 64 MB | 防单文件炸弹 |
| 总解压量 | ≤ 512 MB | 防累积炸弹 |
| 单个 manifest/arsc 读取 | ≤ 32 MB | 防"资源表塞满整个包" |
| 递归深度（ZIP 套 ZIP） | ≤ 2 | `.apks`/`.appxbundle` 够用 |
| 路径白名单 | 只认 `res/ base/res/ Payload/ resources/ Assets/ META-INF/` 及根 manifest | 防路径穿越与意外读 |
| 名称非法字符 | 一律按 ordinal 处理，不落盘 | 防 NTFS 特殊名/ADS |
| 解析总时限 | worker 8 s 自杀 | 防挂死 |

## 9. 实现状态（v1 收口）

| 顺序 | 格式 | 状态 |
| --- | --- | --- |
| 1 | `.apk` | ✅ M1/M2（R-0002/R-0004/R-0005） |
| 2 | `.apks/.xapk/.apkm` | ✅ M4a（R-0011） |
| 3 | `.hap` | ✅ M4d（R-0012；应用名解析 R-0013） |
| — | `.aab` | **不做**（ADR-0006；有实际需求再开） |
| — | `.ipa` / `.appx` / `.msix` | **不做**（ADR-0006） |
