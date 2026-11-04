// 界面文案（中/英）。M5 交付项之一：属性页与 worker 的小工具都要能跟随系统语言。
//
// 用法：
//   Tr(Str::GroupApp)                 -> 当前语言的分组标题
//   TrMsg(L"解析超时")                 -> 解析器内部错误文案的英文版（英文界面时才有意义）
//
// 语言选择顺序：APKSX_LANG=zh|en（脚本/测试用）→ GetUserDefaultUILanguage() 的主语言。
// 只在第一次调用时解析并缓存，之后 SetUiLanguage() 可以强制覆盖（测试用）。
#pragma once

#include <string>
#include <string_view>

namespace apksx {

enum class Lang { Zh, En };

// 界面文案 id：新增一条要同时补 zh/en 两句（i18n.cpp 的表是编译期检查的）
enum class Str {
  PageTitle,

  // 应用组
  GroupApp,
  AppName,
  Package,
  Version,
  LocalesHead,  // "%zu 种 · "
  Locales,

  // 技术组
  GroupTech,
  Module,
  MainAbility,
  ApiVersion,
  ApiLine,  // "min %u / target %u"
  CompileSdk,
  DeviceTypes,
  ComponentAbility,         // "Ability %u"
  ComponentAbilityExt,      // "Ability %u · Extension %u"
  ComponentApk,             // "活动 %u · 服务 %u · 广播 %u · 提供者 %u"
  Components,
  Abi,
  Densities,
  IconSource,
  IconSize,       // 键："图标尺寸"
  IconSizeValue,  // 值："%u × %u"
  IconType,
  IconAdaptive,  // "自适应（XML）"

  // 可折叠明细组
  PermissionsTitle,  // "权限 · %zu 项"
  FeaturesTitle,     // "特性 · %zu 项（%zu 必需）"
  FeatureRequired,   // "（必需）"
  FeatureOptional,   // "（可选）"
  SplitsTitle,       // "分包 · %zu 项"
  SplitKindBase,
  SplitKindConfig,
  SplitKindOther,
  TruncatedTail,  // "…（共 %zu 项）"

  // 文件与签名组
  GroupFiles,
  PackageType,
  PackageTypeHap,   // "HAP（鸿蒙）"
  InnerApk,
  ContainerKind,     // "%s（%u 个 APK）"
  Size,
  Modified,
  BytesGrouped,      // "%s（%s 字节）"
  NotComputed,
  Signature,
  SignatureNone,     // "未发现签名（可能未签名或被重打包）"
  NotImplemented,
  Cert,
  CertDigest,
  Validity,
  OtherCerts,  // "另有 %zu 张证书"

  // 错误态与通用
  ErrorDetails,
  OutputNotObject,  // "解析输出不是 JSON 对象"
  DiagGroup,

  // 属性页状态栏与按钮
  Parsing,
  ParsedDone,     // "解析完成 · %llu ms"
  HashDone,       // "哈希已计算（%llu ms）"
  Hashing,        // "正在计算 SHA-256…"
  ParseFailed,
  MissingWorker,
  BadJson,
  HashMissing,
  UnknownStructure,
  Copied,
  CopiedJson,
  MenuCopyValue,
  MenuCopyPair,
  MenuCopyJson,
  ColumnKey,
  ColumnValue,
  ButtonHash,
  ButtonCopyJson,
  ButtonRetry,
  RetryHint,

  // worker 启动/通信失败
  PipeCreateFailed,
  WorkerStartFailed,
  OutputTooBig,
  Timeout,
  TimeoutAborted,
  NoOutput,
  NotPackage,
  WorkerCrashed,  // "解析器异常退出（0x%08lX）"
};

// 当前语言（首次调用时按 APKSX_LANG / 系统 UI 语言解析）
Lang UiLanguage();

// 强制指定语言（测试与 --lang 用）
void SetUiLanguage(Lang lang);

// 取界面文案（返回的指针指向静态表，不归调用方释放）
const wchar_t* Tr(Str id);

// 解析器内部错误文案的中→英字典：命中就返回英文，未命中原样返回。
// 只在 Lang::En 下生效；中文界面下恒等返回，不做任何查找。
std::wstring TrMsg(std::wstring_view zh);

}  // namespace apksx
