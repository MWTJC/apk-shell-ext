# CI 夹具（入库的小样本）

这 5 个文件是 `tests/fixtures/generated/` 里同名文件的**逐字节副本**，用来让 CI 能跑缩略图 smoke
而不必在 runner 上装 Android SDK/aapt2（aapt2 只在造样本时用，见 `tests/gen_fixtures.py`）。

| 文件 | 大小 | 覆盖的路径 | 期望结果 |
| --- | --- | --- | --- |
| `f_png.apk` | 4.4 KB | PNG 图标 → 密度优先级 → WIC 解码 | 出图 |
| `f_webp.apk` | 1.9 KB | WebP 图标 → WIC WebP 编解码器 | 出图 |
| `f_adaptive.apk` | 5.1 KB | 自适应图标 XML + 退到最高密度 PNG | 出图 |
| `tdeflate_arsc.apk` | 3.2 KB | `resources.arsc` 被 deflate 压缩 | 出图 |
| `f_noicon.apk` | 1.5 KB | 清单里**没有** `android:icon` | **失败（预期）** |

CI 断言写死在 `.github/workflows/build.yml` 的 `Thumbnail smoke` 步骤里：

```
count == 5 && failed == 1 && coldP95 <= 300   (300 是 architecture.md §2.1 的软上限)
```

**改这个目录等于改 CI 基线**：增删文件、或其中任何一个的产出变了，必须同时改上面那条断言与本节表格。
`MANIFEST.json` 里仍有这些包的全部元数据（包名/版本/图标条目/预期 256px 图标路径），断言细节以它为准。
