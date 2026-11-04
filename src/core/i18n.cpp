#include "core/i18n.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace apksx {
namespace {

struct Entry {
  Str id;
  const wchar_t* zh;
  const wchar_t* en;
};

// 顺序无所谓；判重由 tests/propsheet_model_test 的 --selftest 顺手断言"每条都非空"。
constexpr Entry kEntries[] = {
    {Str::PageTitle, L"安装包信息", L"Package info"},

    {Str::GroupApp, L"应用", L"App"},
    {Str::AppName, L"应用名", L"Name"},
    {Str::Package, L"包名", L"Package"},
    {Str::Version, L"版本", L"Version"},
    {Str::LocalesHead, L"%zu 种 · ", L"%zu locales · "},
    {Str::Locales, L"语言", L"Locales"},

    {Str::GroupTech, L"技术", L"Technical"},
    {Str::Module, L"模块", L"Module"},
    {Str::MainAbility, L"主 Ability", L"Main ability"},
    {Str::ApiVersion, L"API 版本", L"API level"},
    {Str::ApiLine, L"min %u / target %u", L"min %u / target %u"},
    {Str::CompileSdk, L"编译 SDK", L"Compile SDK"},
    {Str::DeviceTypes, L"设备类型", L"Device types"},
    {Str::ComponentAbility, L"Ability %u", L"Abilities %u"},
    {Str::ComponentAbilityExt, L"Ability %u · Extension %u", L"Abilities %u · Extensions %u"},
    {Str::ComponentApk, L"活动 %u · 服务 %u · 广播 %u · 提供者 %u",
     L"%u activities · %u services · %u receivers · %u providers"},
    {Str::Components, L"组件", L"Components"},
    {Str::Abi, L"ABI", L"ABI"},
    {Str::Densities, L"密度", L"Densities"},
    {Str::IconSource, L"图标来源", L"Icon source"},
    {Str::IconSize, L"图标尺寸", L"Icon size"},
    {Str::IconSizeValue, L"%u × %u", L"%u × %u"},
    {Str::IconType, L"图标类型", L"Icon type"},
    {Str::IconAdaptive, L"自适应（XML）", L"Adaptive (XML)"},

    {Str::PermissionsTitle, L"权限 · %zu 项", L"Permissions · %zu"},
    {Str::FeaturesTitle, L"特性 · %zu 项（%zu 必需）", L"Features · %zu (%zu required)"},
    {Str::FeatureRequired, L"（必需）", L" (required)"},
    {Str::FeatureOptional, L"（可选）", L" (optional)"},
    {Str::SplitsTitle, L"分包 · %zu 项", L"Splits · %zu"},
    {Str::SplitKindBase, L"base", L"base"},
    {Str::SplitKindConfig, L"配置", L"config"},
    {Str::SplitKindOther, L"分包", L"split"},
    {Str::TruncatedTail, L"…（共 %zu 项）", L"… (%zu total)"},

    {Str::GroupFiles, L"文件与签名", L"File & signature"},
    {Str::PackageType, L"安装包类型", L"Package type"},
    {Str::PackageTypeHap, L"HAP（鸿蒙）", L"HAP (HarmonyOS)"},
    {Str::InnerApk, L"内层 APK", L"Inner APK"},
    {Str::ContainerKind, L"%s（%u 个 APK）", L"%s (%u APKs)"},
    {Str::Size, L"大小", L"Size"},
    {Str::Modified, L"修改时间", L"Modified"},
    {Str::BytesGrouped, L"%s（%s 字节）", L"%s (%s bytes)"},
    {Str::NotComputed, L"未计算", L"not computed"},
    {Str::Signature, L"签名", L"Signature"},
    {Str::SignatureNone, L"未发现签名（可能未签名或被重打包）",
     L"No signature found (unsigned or repacked)"},
    {Str::NotImplemented, L"未实现", L"not implemented"},
    {Str::Cert, L"证书", L"Certificate"},
    {Str::CertDigest, L"证书摘要", L"Cert digest"},
    {Str::Validity, L"有效期", L"Validity"},
    {Str::OtherCerts, L"另有 %zu 张证书", L"%zu more certificates"},

    {Str::ErrorDetails, L"详情", L"Details"},
    {Str::OutputNotObject, L"解析输出不是 JSON 对象", L"Parser output is not a JSON object"},
    {Str::DiagGroup, L"诊断", L"Diagnostics"},

    {Str::Parsing, L"正在解析…", L"Parsing…"},
    {Str::ParsedDone, L"解析完成 · %llu ms", L"Parsed · %llu ms"},
    {Str::HashDone, L"哈希已计算（%llu ms）", L"SHA-256 computed (%llu ms)"},
    {Str::Hashing, L"正在计算 SHA-256…", L"Computing SHA-256…"},
    {Str::ParseFailed, L"解析失败", L"Parse failed"},
    {Str::MissingWorker, L"解析器缺失：apkinfo.exe 不在扩展目录里",
     L"Parser missing: apkinfo.exe is not next to the extension DLL"},
    {Str::BadJson, L"解析输出不是合法 JSON", L"Parser output is not valid JSON"},
    {Str::HashMissing, L"哈希计算没有返回 sha256", L"Hash run returned no sha256"},
    {Str::UnknownStructure, L"解析输出结构不认识", L"Unrecognised parser output"},
    {Str::Copied, L"已复制到剪贴板", L"Copied to clipboard"},
    {Str::CopiedJson, L"已复制 JSON 到剪贴板", L"JSON copied to clipboard"},
    {Str::MenuCopyValue, L"复制值", L"Copy value"},
    {Str::MenuCopyPair, L"复制“属性: 值”", L"Copy “key: value”"},
    {Str::MenuCopyJson, L"复制全部为 JSON", L"Copy all as JSON"},
    {Str::ColumnKey, L"属性", L"Property"},
    {Str::ColumnValue, L"值", L"Value"},
    {Str::ButtonHash, L"计算 SHA-256", L"Compute SHA-256"},
    {Str::ButtonCopyJson, L"复制全部为 JSON", L"Copy all as JSON"},
    {Str::ButtonRetry, L"重试", L"Retry"},
    {Str::RetryHint, L"可以点「重试」再解析一次。", L"Click “Retry” to parse again."},

    {Str::PipeCreateFailed, L"创建管道失败", L"Failed to create the pipe"},
    {Str::WorkerStartFailed, L"无法启动解析器（apkinfo.exe）", L"Failed to start apkinfo.exe"},
    {Str::OutputTooBig, L"解析输出超过 4 MB（异常）", L"Parser output exceeded 4 MB"},
    {Str::Timeout, L"解析超时", L"Parse timed out"},
    {Str::TimeoutAborted, L"解析超时（已由解析器中止）", L"Parse timed out (parser aborted)"},
    {Str::NoOutput, L"解析器没有输出", L"Parser produced no output"},
    {Str::NotPackage, L"不是有效的安装包", L"Not a valid package"},
    {Str::WorkerCrashed, L"解析器异常退出（0x%08lX）", L"Parser crashed (0x%08lX)"},
};

// 解析器内部错误文案（core/*.cpp 里 err->set(...) 的原文）→ 英文。
// 只覆盖会经 apkinfo stderr 传到用户眼前的那批；WIC/编码器内部错误只进调试日志，不翻译。
struct MsgPair {
  const wchar_t* zh;
  const wchar_t* en;
};

constexpr MsgPair kMessages[] = {
    {L"解析失败", L"parse failed"},
    {L"解析超时", L"parse timed out"},
    {L"无法打开文件", L"cannot open file"},
    {L"无法获取文件大小", L"cannot stat file size"},
    {L"无法获取 stream 大小", L"cannot stat stream size"},
    {L"stream 为空", L"empty stream"},
    {L"数据源为空", L"empty data source"},
    {L"无法读取文件属性", L"cannot read file attributes"},
    {L"计算 SHA-256 失败", L"SHA-256 failed"},
    {L"无法打开文件计算哈希", L"cannot open file for hashing"},

    {L"文件太小，不是有效 ZIP", L"file too small to be a ZIP"},
    {L"未找到 ZIP 结束记录（不是有效 ZIP）", L"no ZIP end-of-directory record (not a ZIP)"},
    {L"ZIP 中央目录为空", L"empty ZIP central directory"},
    {L"ZIP 中央目录越界", L"ZIP central directory out of bounds"},
    {L"读取中央目录失败", L"cannot read central directory"},
    {L"中央目录里没有可用条目", L"no usable entry in the central directory"},
    {L"ZIP 条目数超限（>65535）", L"too many ZIP entries (>65535)"},
    {L"条目已加密", L"entry is encrypted"},
    {L"条目数据越界", L"entry data out of bounds"},
    {L"条目解压后超过上限", L"entry exceeds the decompression limit"},
    {L"不支持的压缩方法", L"unsupported compression method"},
    {L"deflate 解压失败（数据损坏或超限）", L"deflate failed (corrupt data or over limit)"},
    {L"读取条目数据失败", L"cannot read entry data"},
    {L"本地文件头无效", L"invalid local file header"},
    {L"ZIP64 定位器缺失", L"ZIP64 locator missing"},
    {L"ZIP64 定位器无效", L"invalid ZIP64 locator"},
    {L"ZIP64 结束记录无效", L"invalid ZIP64 end record"},
    {L"读取文件尾部失败", L"cannot read file tail"},

    {L"AXML 文件太小", L"AXML too small"},
    {L"不是二进制 XML（AXML）", L"not binary XML (AXML)"},
    {L"AXML 缺少字符串池", L"AXML has no string pool"},
    {L"AXML 里没有元素", L"AXML has no elements"},

    {L"resources.arsc 太小", L"resources.arsc too small"},
    {L"不是 RES_TABLE", L"not a RES_TABLE"},
    {L"arsc 数据源为空", L"empty arsc source"},
    {L"字符串池头损坏", L"corrupt string pool header"},
    {L"字符串池过小", L"string pool too small"},
    {L"不是字符串池 chunk", L"not a string pool chunk"},
    {L"字符串池布局不一致", L"inconsistent string pool layout"},
    {L"字符串池偏移表越界", L"string pool offset table out of bounds"},
    {L"字符串池数据区越界", L"string pool data out of bounds"},
    {L"JSON 解析失败", L"JSON parse failed"},
    {L"读取清单超时", L"manifest read timed out"},
    {L"解析资源表超时", L"resource table read timed out"},

    {L"包内缺少 AndroidManifest.xml（不是有效 APK）", L"no AndroidManifest.xml (not an APK)"},
    {L"清单里没有 <application>", L"manifest has no <application>"},
    {L"应用未声明 android:icon", L"no android:icon declared"},
    {L"只有 adaptive icon（M1 不支持合成）", L"adaptive icon only (layers not merged)"},
    {L"未找到可用的 launcher 图标", L"no usable launcher icon"},
    {L"包里没有可用的 launcher 图标", L"no usable launcher icon in the package"},
    {L"包内既没有 AndroidManifest.xml，也没有内层 .apk",
     L"neither AndroidManifest.xml nor an inner .apk"},

    {L"容器里找不到 base.apk", L"no base.apk in the container"},
    {L"容器里的 base.apk 打不开（中央目录损坏）", L"container base.apk unreadable (corrupt CD)"},
    {L"内层 base.apk 打不开（中央目录损坏）", L"inner base.apk unreadable (corrupt CD)"},
    {L"内层 base.apk 是 deflate 且超过 512 MB，v1 不解压",
     L"inner base.apk is deflated and over 512 MB (v1 does not inflate)"},

    {L"HAP 清单读不出来", L"cannot read the HAP manifest"},
    {L"module.json 读不出来", L"cannot read module.json"},
    {L"module.json 不是合法 JSON", L"module.json is not valid JSON"},
    {L"module.json 缺少 app / module 段", L"module.json has no app/module section"},
    {L"module.json 里没有 bundleName", L"module.json has no bundleName"},
    {L"HAP 里找不到可用图标", L"no usable icon in the HAP"},
    {L"HAP 里没有可用图标", L"no usable icon in the HAP"},
    {L"图标条目读不出来", L"cannot read the icon entry"},
    {L"图标条目是空的", L"icon entry is empty"},

    {L"图像数据为空", L"empty image data"},
    {L"没有可用的图像", L"no usable image"},
    {L"图像尺寸无效", L"invalid image size"},
};

Lang g_lang = Lang::Zh;
bool g_resolved = false;

bool IsZhUi() {
  // 1) 用户在 HKCU 里的显式选择（apkinfo --lang zh|en|auto 写它）：
  //    属性页跑在 explorer.exe 里，改不了它的环境变量，所以必须有一个进程外的开关。
  {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\apk-shell-ext", 0,
                        KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
      wchar_t value[16] = {};
      DWORD bytes = sizeof(value);
      DWORD type = 0;
      const bool ok = ::RegQueryValueExW(key, L"UiLanguage", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(value), &bytes) ==
                      ERROR_SUCCESS;
      ::RegCloseKey(key);
      if (ok && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        if (_wcsicmp(value, L"en") == 0) return false;
        if (_wcsicmp(value, L"zh") == 0) return true;
      }
    }
  }
  // 2) 环境变量（脚本、测试、演示用）
  wchar_t env[16] = {};
  if (::GetEnvironmentVariableW(L"APKSX_LANG", env, ARRAYSIZE(env)) > 0) {
    if (_wcsicmp(env, L"en") == 0) return false;
    if (_wcsicmp(env, L"zh") == 0) return true;
  }
  // 3) 系统 UI 语言：只认"中文"这一族，其它语言一律走英文文案
  const LANGID lang = ::GetUserDefaultUILanguage();
  return (lang & 0x3FF) == 0x04;
}

}  // namespace

Lang UiLanguage() {
  if (!g_resolved) {
    g_lang = IsZhUi() ? Lang::Zh : Lang::En;
    g_resolved = true;
  }
  return g_lang;
}

void SetUiLanguage(Lang lang) {
  g_lang = lang;
  g_resolved = true;
}

const wchar_t* Tr(Str id) {
  const bool zh = UiLanguage() == Lang::Zh;
  for (const Entry& entry : kEntries) {
    if (entry.id == id) return zh ? entry.zh : entry.en;
  }
  return L"";  // 不认识的 id：空串（界面上表现为缺字段，比崩溃好）
}

std::wstring TrMsg(std::wstring_view zh_text) {
  if (UiLanguage() == Lang::Zh) return std::wstring(zh_text);
  for (const MsgPair& pair : kMessages) {
    if (zh_text == pair.zh) return pair.en;
  }
  return std::wstring(zh_text);
}

}  // namespace apksx
