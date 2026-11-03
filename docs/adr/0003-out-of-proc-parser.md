# ADR-0003 · 全量解析放在独立 worker 进程

- 状态：**已采纳**
- 日期：2026-11-03
- 相关：[architecture.md §1/§2.3](../architecture.md)、[design.md §P2](../design.md)

## 上下文

属性页 handler 是 **in-proc COM**，DLL 被加载进 `explorer.exe`（HashTab 与果核看图都是 `ThreadingModel=Apartment` 的进程内 server `[实测]`）。这带来三个硬问题：

1. **崩溃即桌面崩**：一个 `std::bad_alloc`、一个未捕获的 `std::filesystem_error`，Explorer 就重启。
2. **依赖即桌面依赖**：DLL 若动态链接 MFC/Qt/VC 运行库，缺库时整个扩展静默失效（果核 thumb dll 动态依赖 `MSVCP140.dll` `[实测]`）。
3. **常驻即桌面内存**：解析器的数据结构、protobuf/plist 运行时都会留在 explorer 地址空间。

而属性对话框是**模态**的，会把 explorer 的 UI 线程锁住 `[上游]`——同步解析几百 MB 的包就是"未响应"。

## 决策

1. 全量解析（AXML/ARSC/签名/plist/protobuf/鸿蒙 index/DEX 扫描）只存在于 `apkinfo.exe`。
2. 通信方式：`CreateProcessW` + 匿名管道（stdout 读 JSON），**不做 COM LocalServer / surrogate**。
3. `apkprop.dll` 只负责：建页、起进程、读管道、回填控件、销毁时取消（`TerminateProcess` 兜底）。
4. 缩略图路径**不**走 worker（v1 例外，理由见下）。

## 后果

**正面**

- 崩溃隔离：worker 挂掉只影响一个属性页，Explorer 不受影响（V7 有专门用例）。
- 依赖隔离：protobuf / plist / 正则等重依赖只在 worker 里，explorer 只认系统 DLL。
- 可以放心用大内存、大超时的解析策略，不必担心拖累桌面。
- 同一份解析器给三个调用方复用：属性页、`apkinfo.exe` 命令行（用户/脚本可直接用）、测试。

**负面**

- 每次打开属性页多 ~20–40 ms 进程启动（可用"常驻小守护"优化，但 v1 不做）。
- 需要定义稳定的 JSON 契约（schema v1），未来演进受约束。
- 取消语义要自己实现（终止进程 = 放弃结果，需保证不留半写缓存）。

## 为什么缩略图例外

缩略图有 300 ms 预算，进程启动就吃掉 1/5，且频率虽低但覆盖整个目录（100 个包 × 20 ms 启动 = 2 s 额外）。因此缩略图在 DLL 内只做"中央目录 + 单条目解码"，代码量极小（无 protobuf/plist），风险可控。

M1 实测若发现 WIC 解码 + ARSC 分段读让 DLL 体积/耗时超标，可另立 ADR 把缩略图也搬到 worker（届时接受进度条式占位图标）。

## 替代方案

| 方案 | 否决原因 |
| --- | --- |
| 全在 DLL 内解析 | 崩溃/依赖/内存三风险全背；与 ADR-0004 的依赖边界冲突 |
| COM LocalServer / DllSurrogate | 需要注册 `AppID`、配置 surrogate，权限与调试都更麻烦；收益只是"看起来像 COM" |
| 后台线程 + 同进程 | 崩溃风险不减（线程崩同样带崩进程），大内存仍在 explorer |
| 常驻守护进程 + 共享内存 | 启动快，但引入常驻进程、生命周期与更新一致性问题；v1 不做 |
