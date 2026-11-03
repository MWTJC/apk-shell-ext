# 安装包格式矩阵与解析要点

> 目标：把"同一件事在不同格式里叫什么、藏在哪里"写成表，避免实现时逐个踩坑。
> 标注：`[实测]` 本机样本；其余为格式规范/官方文档推断，M2/M4 逐项用真实包验证后再改状态。

## 1. 总表

| 格式 | 容器 | manifest | 资源表 | 图标位置 | 难度 | 阶段 |
| --- | --- | --- | --- | --- | --- | --- |
| `.apk` | ZIP | `AndroidManifest.xml`（**二进制 AXML**） | `resources.arsc`（二进制） | `res/mipmap-*/ic_launcher.*`，adaptive 在 `mipmap-anydpi-v26/*.xml` | 中 | M1/M2 |
| `.apks` / `.xapk` / `.apkm` | ZIP 套 ZIP | 内层 `base.apk` | 同上 | 同上（取 base） | 低（多一层） | M4 |
| `.aab` | ZIP | `base/manifest/AndroidManifest.xml`（**protobuf**，Bundle 格式） | `base/resources.pb`（protobuf） | `base/res/mipmap-*/…`，索引在 `resources.pb` | **高** | M4 |
| `.ipa` | ZIP | `Payload/*.app/Info.plist`（XML 或 binary plist） | 无 | `Payload/*.app/AppIcon*.png` 或 `Assets.car` | 中高（`.car`） | M4 |
| `.hap` | ZIP | `module.json`（JSON） | `resources.index`（**鸿蒙二进制索引**） | `resources/base/media/*.png`，由 `$media:` 引用 | 高 | M4 |
| `.appx` / `.msix` | ZIP | `AppxManifest.xml`（XML） | 无 | `Assets\Square44x44Logo*.png`（含 scale/contrast 限定符） | 低 | M4 |
| `.appxbundle` | ZIP 套 ZIP | 内层 `*_x64.appx` | 无 | 同上 | 低 | M4（可选） |

**关键差异**：AAB 的 manifest 不是 AXML 而是 **protobuf**；HAP 的资源不是 `arsc` 而是鸿蒙自己的 `resources.index`。这两条决定了"一个 ZIP+AXML 解析器通吃"的假设不成立，必须分开写 reader。

## 2. APK（M1/M2 主战场）

**读取顺序（缩略图只需 1–3 + 5）**

1. **EOCD → 中央目录**：`zip` 结尾 `PK\x05\x06`，注意 ZIP64（大包）。条目名按 ordinal 索引，避免编码/大小写问题（ApkShellext2-Plus 修的正是这类毒包）。
2. **`AndroidManifest.xml`**：AXML 头 `0x00080003`；取 `<application>` 的 `android:icon`（可能是资源引用 `@0x7f…`，也可能是字面路径）。
3. **`resources.arsc`**：`RES_TABLE_TYPE` → `RES_STRING_POOL_TYPE`（全局字符串池）→ `RES_TABLE_PACKAGE_TYPE`（按 package id，通常是 0x7f）→ 目标 `RES_TABLE_TYPE_TYPE`（每种 density 一组）。
4. **签名**：v2/v3 在 `APK Signing Block`（中央目录前），v1 在 `META-INF/*.RSA`。v1 解析代价高，属性页只做"方案列表 + 证书摘要"，证书用 `CryptQueryObject`/`CertGetNameString`。
5. **图标候选**：优先 `mipmap`，其次 `drawable`；density 优先级 xxxhdpi > xxhdpi > xhdpi > hdpi > mdpi > nodpi > anydpi（anydpi 通常是 adaptive XML）。
6. **adaptive icon**：`<adaptive-icon><background/><foreground/></adaptive-icon>`，两层都是资源引用（可能是颜色或图片）。M2 支持合成（前景居中叠背景），失败回退纯前景。

**split APK 处理**：`.apks/.xapk/.apkm` 外层有 `base.apk` + `split_config.*.apk` + 可选 `icon.png`/`manifest.json`。v1 只读 `base.apk`；若 base 缺失（少见），按包名合并取最大 split。安装体积/ABI 列表在属性页才汇总。

## 3. AAB（Android App Bundle）

- 结构：`BundleConfig.pb` + `base/manifest/AndroidManifest.xml` + `base/resources.pb` + `base/res/…` + 各模块目录（`feature1/…`）。
- **manifest 与资源表都是 protobuf**（`Resources.proto` / `Bundle` 的 `XmlNode` 定义），不能用 AXML 解析器。方案：把 `.proto` 编译进 worker（`protobuf-lite` 或 hand-written decoder），M4 先只支持 `base` 模块。
- 图标：manifest 里 `android:icon` 同样是资源引用 → 在 `resources.pb` 里查 ID → 得到 `base/res/mipmap-*/…` 路径 → 从 ZIP 读。
- 属性页可额外显示：模块列表（base/feature）、`delivery` 条件（设备/语言/密度）。

## 4. IPA

- `Payload/<Name>.app/Info.plist`：可能是 XML plist，也可能是 **binary plist**（`bplist00`）；两种都要解。
- 图标优先级：`CFBundleIcons` / `CFBundleIcons~ipad` 里声明的文件名 → `AppIcon60x60@2x.png` 等约定名 → `iTunesArtwork@2x` → **`Assets.car`**（编译后的资产目录，需解析 CgBI/asset catalog 格式）。
- `Assets.car` 是**唯一的高清来源**（App Store 包通常把 PNG 删掉只留 car）。M4 先做"约定名 + iTunesArtwork"回退，car 解析单列 M4b（可先只支持 `AppIcon` 组里最大尺寸）。
- 签名：`_CodeSignature/CodeResources`（可只显示"存在/缺失"，不做完整校验）。

## 5. HAP（HarmonyOS）

- 结构：`module.json`（模块元数据，JSON）+ `pack.info` + `resources.index`（二进制）+ `resources/base/media/…` + `.abc`（ArkTS 字节码）。
- `module.json` 里 `app.bundleName` / `module.name` / `versionCode` / `versionName` / `minAPIVersion` / `targetAPIVersion` / `deviceTypes`。
- 图标：`module.json` 的 `abilities[].icon` 形如 `$media:app_icon` → 在 `resources.index` 里查到资源 ID → 映射到 `resources/base/media/app_icon.png`。
- **`resources.index` 是鸿蒙自有格式**（非 `arsc`），需要按官方 `ResourceTable` 结构解析；M4 先做"按 `$media:` 名字直接猜 `resources/base/media/<name>.*`"的启发式（覆盖大多数包），索引解析放 M4b。
- 分模块包（`.hsp` / `.app` 套件）v1 不做。

## 6. APPX / MSIX

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

## 9. 实现优先级

| 顺序 | 格式 | 理由 |
| --- | --- | --- |
| 1 | `.apk` | 覆盖 80% 场景，且 M1 就要用 |
| 2 | `.apks/.xapk/.apkm` | 只多一层 ZIP，复用 APK reader |
| 3 | `.aab` | 开发者常用，但需要 protobuf，单独排期 |
| 4 | `.ipa` | 图标来源复杂（Assets.car），先做回退路径 |
| 5 | `.hap` | 资源索引非标准，先启发式 |
| 6 | `.appx/.msix` | 最简单，顺手做 |
