# ADR-0005 · 缩略图在隔离宿主（dllhost）里跑，输入是 IStream

- 状态：**已采纳**（M1 实测迫使修订 ADR-0003 的"缩略图例外"细节）
- 日期：2026-11-03
- 相关：[design.md §P2](../design.md)、[architecture.md §1/§2.1](../architecture.md)、[ADR-0003](0003-out-of-proc-parser.md)、[verification.md R-0002](../verification.md)

## 背景（实测，不是推断）

M1 实现完成后，按 verification.md 的配方用 `IShellItemImageFactory::GetImage(SIIGBIF_THUMBNAILONLY)`
驱动系统缩略图管线（与 Explorer 同一条路径），得到三个事实：

| 观测 | 证据 |
| --- | --- |
| 缩略图 handler **不在 explorer.exe 里跑** | `Get-Process \| ? Modules -match apkthumb.dll` → 命中进程是 **dllhost.exe**（COM surrogate），explorer.exe 未加载该 DLL |
| 系统喂进来的是 **IStream**，不是 IShellItem / 路径 | 只实现 `IInitializeWithItem` + `IInitializeWithFile` 时，`GetImage` 直接返回 `0x8004B200`（WTS_E_FAILEDEXTRACTION），我们的代码一行都没执行；补上 `IInitializeWithStream` 后才进入 `GetThumbnail` |
| 流上**没有可用路径** | `IStream::Stat(STATFLAG_DEFAULT).pwcsName` 返回的是裸文件名（如 `shell_a.apk`），宿主进程 CWD 是 `C:\WINDOWS\system32`，拼不出真实路径 |

这与 [design.md §P2](../design.md) 当初"handler 直接进 explorer 地址空间"的判断**不一致**。
不影响结论（越隔离越安全），但影响实现：按路径打开文件的写法在 Explorer 里必然失效。

## 决策

1. 缩略图 handler **同时实现** `IInitializeWithItem`、`IInitializeWithFile`、`IInitializeWithStream`；
   三者都归结为一件事：为解析核心准备一个 `ByteSource`。
   - item/file → `FileSource`（Win32 句柄，随机读）。
   - stream → `StreamSource`（IStream Clone + Seek/Read，随机读；不整包读入内存）。
2. 解析核心（ZIP 中央目录 / AXML / ARSC / 图标条目）只依赖 `ByteSource`，不依赖路径。
   因此"只读中央目录 + 单条目"的 IO 模型在任何宿主下都成立。
3. 缓存键分两种：
   - 文件模式：`sha1(path | size | mtime | 解析器版本)`（原设计不变，命中路径零 CreateFile）。
   - 流模式：`sha1(name | size | mtime | 中央目录 SHA-1 | 解析器版本)`。
     **中央目录摘要必须进键**——流只有裸文件名，不同目录下的同名包否则会串图。
4. 缩略图路径仍然**不经过 worker 进程**（ADR-0003 的例外保持）：现在多出的代价只是
   "打开流 + 读中央目录"，与路径模式同量级，M1 实测 P95 159 ms 仍在 300 ms 预算内。

## 后果

**正面**

- 在真实 Explorer 管线上出图（已用 `--shell` 模式验证，含真实包）。
- 隔离宿主反而降低了"我们的 bug 弄崩桌面"的风险。
- 解析核心与 IO 解耦，M2 的 `apkinfo.exe` 可以直接复用同一份核心（文件模式），
  将来若要做"从 adb 管道流式解析"也只是再加一个 `ByteSource`。

**负面 / 需要接受**

- 流模式无法用 `GetFileAttributesEx` 做"零打开"快速判命中：必须先读中央目录才能算键。
  代价是一次几十 KB 的顺序读（实测 ≤ 3 ms，含在 P50 8 ms 里）。
- 流模式的缓存键依赖中央目录内容，同一文件被重新打包（内容不变但 CD 布局变）会各占一份缓存；
  200 MB 上限 + LRU 清理可以兜住。

## 替代方案

| 方案 | 否决原因 |
| --- | --- |
| 只实现 `IInitializeWithItem` | 实测在 explorer 的隔离管线里根本不调用（0x8004B200），功能不可用 |
| 从流里读全包到临时文件再按路径解析 | 违背 design.md §7.6"不解压到临时目录"；大包会把 IO 翻几倍 |
| 把缩略图也搬进 worker 进程 | 仍要通过流把数据送过去，等于多一次 IPC 与一次内存拷贝；ADR-0003 的例外理由（300 ms 预算）不变 |
| 关闭缩略图隔离（注册 `DisableProcessIsolation`） | 用"更不隔离"换"少写代码"，与 P2 的安全方向相反；且需要写额外注册表值 |

## 修订条件

若将来 Windows 改为把缩略图 handler 放回 explorer 进程内，本 ADR 的实现（流 + 文件双模式）
仍然可用，只是流分支可能不再被触发；此时无需改代码，只需更新文档中的实测记录。
