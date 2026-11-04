#include "core/apk_info.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <utility>

#include "core/apk_icon.h"
#include "core/arsc.h"
#include "core/arsc_stream.h"
#include "core/axml.h"
#include "core/file_util.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

constexpr const char* kManifestEntry = "AndroidManifest.xml";
constexpr const char* kArscEntry = "resources.arsc";

std::string Lower(std::string_view in) {
  std::string out(in);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool IsKnownAbi(std::string_view abi) {
  static const char* kAbis[] = {"armeabi-v7a", "armeabi", "arm64-v8a", "x86",
                                "x86_64",      "mips",    "mips64",    "riscv64"};
  for (const char* a : kAbis) {
    if (abi == a) return true;
  }
  return false;
}

int DensityOrder(const std::string& name) {
  static const struct {
    const char* name;
    int order;
  } kTable[] = {{"xxxhdpi", 0}, {"xxhdpi", 1}, {"xhdpi", 2}, {"hdpi", 3},
                {"mdpi", 4},    {"ldpi", 5},   {"nodpi", 6},  {"anydpi", 7}};
  for (const auto& t : kTable) {
    if (name == t.name) return t.order;
  }
  return 99;
}

std::string LocaleTag(const ArscConfig& c) {
  if (c.language.empty()) return std::string();
  std::string tag = c.language;
  if (!c.country.empty()) {
    tag += '-';
    tag += c.country;
  }
  return tag;
}

// 从 PNG 的 IHDR 取宽高（图标尺寸的廉价来源：不用解码整张图）
void PngSize(const std::vector<uint8_t>& bytes, uint32_t* w, uint32_t* h) {
  if (bytes.size() < 24) return;
  if (!(bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')) return;
  const uint8_t* p = bytes.data() + 16;
  *w = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  *h = (static_cast<uint32_t>(p[4]) << 24) | (static_cast<uint32_t>(p[5]) << 16) |
       (static_cast<uint32_t>(p[6]) << 8) | static_cast<uint32_t>(p[7]);
}

}  // namespace

std::string DensityName(uint16_t density) {
  switch (density) {
    case kDensityLow: return "ldpi";
    case kDensityMedium: return "mdpi";
    case kDensityHigh: return "hdpi";
    case kDensityXHigh: return "xhdpi";
    case kDensityXXHigh: return "xxhdpi";
    case kDensityXXXHigh: return "xxxhdpi";
    case kDensityAny: return "anydpi";
    case kDensityNone: return "nodpi";
    case kDensityDefault: return std::string();
    default: return std::to_string(density);
  }
}

// 一个 APK（单包，或容器里的 base）的全部字段。
// 调用方负责把正确的 ZIP 喂进来：单 .apk 就是文件本身，容器是 base.apk 的窗口/内存块。
static bool ScanApkZip(ZipReader& zip, ApkInfo* out, Error* err) {
  if (out == nullptr) return false;
  *out = ApkInfo();

  const Deadline deadline(0);  // 总时限由 CLI 的 --timeout 负责

  // ---------------------------------------------------------------- 清单（只解析一次）
  const ZipEntry* manifest_entry = zip.Find(kManifestEntry);
  if (manifest_entry == nullptr) {
    if (err) err->set(L"包内缺少 AndroidManifest.xml（不是有效 APK）");
    return false;
  }
  std::vector<uint8_t> manifest_bytes;
  if (!zip.Read(*manifest_entry, kMaxManifestRead, &manifest_bytes, err)) return false;

  std::vector<AxmlElement> elements;
  if (!ParseAxml(manifest_bytes, &elements, err)) return false;

  ManifestInfo manifest;
  if (!ParseManifestElements(elements, &manifest, err)) return false;
  out->package = manifest.package;
  out->label = manifest.label;
  out->version_name = manifest.version_name;
  out->version_code = manifest.version_code;
  out->min_sdk = manifest.min_sdk;
  out->target_sdk = manifest.target_sdk;

  // 组件计数、权限，以及 label 的资源引用
  std::set<std::string> perms;
  std::set<std::string> seen_features;
  uint32_t label_res_id = kInvalidResId;
  for (const AxmlElement& el : elements) {
    if (el.tag == "activity" || el.tag == "activity-alias") {
      ++out->components.activities;
    } else if (el.tag == "service") {
      ++out->components.services;
    } else if (el.tag == "receiver") {
      ++out->components.receivers;
    } else if (el.tag == "provider") {
      ++out->components.providers;
    } else if (el.tag == "uses-permission" || el.tag == "uses-permission-sdk-23") {
      std::string name;
      if (GetStringAttr(el, kAttrName, "name", &name) && !name.empty()) perms.insert(name);
    } else if (el.tag == "uses-feature") {
      std::string name;
      if (GetStringAttr(el, kAttrName, "name", &name) && !name.empty() &&
          seen_features.insert(name).second) {
        ApkFeature feature;
        feature.name = name;
        // android:required 缺省 true；写错的清单可能给字符串 "false"，一并认
        feature.required = true;
        if (const AxmlAttr* a = el.Find(kAttrRequired, "required")) {
          if (a->type == kResTypeIntBoolean || a->type == kResTypeIntDec ||
              a->type == kResTypeIntHex) {
            feature.required = a->data != 0;
          } else if (a->has_string) {
            feature.required = a->string_value != "false";
          }
        }
        out->features.push_back(std::move(feature));
      }
    } else if (el.tag == "application") {
      const AxmlAttr* a = el.Find(kAttrLabel, "label");
      if (a != nullptr && a->type == kResTypeReference) label_res_id = a->data;
    }
  }
  out->permissions.assign(perms.begin(), perms.end());

  // ---------------------------------------------------------------- 资源表
  // 首选流式（TD-001 关闭）：Stored arsc 直接开窗口，按 chunk 跳读；
  // 只有 deflate 的 arsc 才回退到有界整读（deflate 无法随机访问，见 TD-002）。
  ArscTable memory_table;
  ArscStreamTable stream_table;
  bool have_arsc = false;
  bool streaming = false;
  const ZipEntry* arsc_entry = zip.Find(kArscEntry);
  if (arsc_entry == nullptr) {
    out->warnings.push_back("包内没有 resources.arsc");
  } else {
    if (auto stored = zip.OpenStoredEntry(*arsc_entry)) {
      streaming = stream_table.Open(stored, nullptr);
      have_arsc = streaming;
    }
    if (!have_arsc) {
      if (arsc_entry->uncompressed_size > kMaxArscRead) {
        out->warnings.push_back(
            "resources.arsc 被 deflate 且超过 8 MB，已跳过（TD-002：deflate 不支持分段读）");
      } else {
        std::vector<uint8_t> arsc_bytes;
        if (zip.Read(*arsc_entry, kMaxArscRead, &arsc_bytes, nullptr)) {
          have_arsc = memory_table.Parse(arsc_bytes, nullptr);
        }
      }
    }
    if (!have_arsc && arsc_entry->uncompressed_size <= kMaxArscRead) {
      out->warnings.push_back("resources.arsc 解析失败");
    }
  }

  auto collect_candidates = [&](uint32_t id, std::vector<ArscCandidate>* cands) {
    if (streaming) {
      stream_table.CollectCandidates(id, cands);
    } else if (have_arsc) {
      memory_table.CollectCandidates(id, cands);
    }
  };
  auto collect_configs = [&](std::vector<ArscConfig>* configs) {
    if (streaming) {
      stream_table.CollectConfigs(configs);
    } else if (have_arsc) {
      memory_table.CollectConfigs(configs);
    }
  };

  // label：清单里是资源引用时，取"默认配置"的那条字符串
  if (out->label.empty() && have_arsc && label_res_id != kInvalidResId) {
    std::vector<ArscCandidate> cands;
    collect_candidates(label_res_id, &cands);
    std::sort(cands.begin(), cands.end(), [](const ArscCandidate& x, const ArscCandidate& y) {
      const bool x_def = x.language.empty() && x.country.empty();
      const bool y_def = y.language.empty() && y.country.empty();
      if (x_def != y_def) return x_def;
      return x.value_string < y.value_string;
    });
    for (const ArscCandidate& c : cands) {
      if (!c.value_string.empty()) {
        out->label = c.value_string;
        break;
      }
    }
  }

  if (have_arsc) {
    std::vector<ArscConfig> configs;
    collect_configs(&configs);
    std::set<std::string> locales;
    std::set<std::string> densities;
    for (const ArscConfig& c : configs) {
      const std::string tag = LocaleTag(c);
      if (!tag.empty()) locales.insert(tag);
      const std::string dn = DensityName(c.density);
      if (!dn.empty()) densities.insert(dn);
    }
    out->locales.assign(locales.begin(), locales.end());
    out->densities.assign(densities.begin(), densities.end());
  }

  // ---------------------------------------------------------------- 中央目录
  std::set<std::string> abis;
  std::set<std::string> path_densities;
  for (const ZipEntry& e : zip.entries()) {
    if (StartsWith(e.name, "lib/")) {
      const size_t slash = e.name.find('/', 4);
      if (slash != std::string::npos) {
        const std::string abi = e.name.substr(4, slash - 4);
        if (IsKnownAbi(abi)) abis.insert(abi);
      }
    }
    const std::string lower = Lower(e.name);
    if (!StartsWith(lower, "res/") && !StartsWith(lower, "base/res/")) continue;
    const size_t dir_end = lower.find('/');
    if (dir_end == std::string::npos) continue;
    const size_t dash = lower.find('-', dir_end);
    if (dash == std::string::npos) continue;
    const size_t token_end = lower.find_first_of("/-", dash + 1);
    const std::string token = lower.substr(dash + 1, token_end - dash - 1);
    if (token.empty()) continue;
    for (const char* d : {"xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi", "ldpi", "nodpi",
                          "anydpi"}) {
      if (token == d) {
        path_densities.insert(d);
        break;
      }
    }
  }
  out->abis.assign(abis.begin(), abis.end());
  for (const std::string& d : path_densities) {
    if (std::find(out->densities.begin(), out->densities.end(), d) == out->densities.end()) {
      out->densities.push_back(d);
    }
  }
  std::sort(out->densities.begin(), out->densities.end(),
            [](const std::string& a, const std::string& b) {
              return DensityOrder(a) < DensityOrder(b);
            });

  // ---------------------------------------------------------------- 签名
  // 只读中央目录前的签名块（几 KB）与 META-INF 的 PKCS#7，不做摘要校验。
  CollectApkSignature(zip, &out->signature, &out->warnings);

  // ---------------------------------------------------------------- 图标
  IconLocation icon;
  if (LocateIconInZip(zip, manifest, &icon, nullptr, &deadline)) {
    out->icon_source = icon.entry_path;
    out->icon_adaptive = icon.adaptive_xml;
    const ZipEntry* icon_entry = zip.Find(icon.entry_path);
    if (icon_entry != nullptr) {
      std::vector<uint8_t> icon_bytes;
      if (zip.Read(*icon_entry, kMaxIconRead, &icon_bytes, nullptr)) {
        PngSize(icon_bytes, &out->icon_width, &out->icon_height);
        const bool png = icon_bytes.size() >= 8 && icon_bytes[0] == 0x89 && icon_bytes[1] == 'P';
        const bool webp = icon_bytes.size() >= 12 && icon_bytes[0] == 'R' &&
                          icon_bytes[1] == 'I' && icon_bytes[2] == 'F' && icon_bytes[3] == 'F' &&
                          icon_bytes[8] == 'W' && icon_bytes[9] == 'E' && icon_bytes[10] == 'B' &&
                          icon_bytes[11] == 'P';
        const bool jpeg = icon_bytes.size() >= 3 && icon_bytes[0] == 0xFF &&
                          icon_bytes[1] == 0xD8;
        if (!png && !webp && !jpeg) {
          out->warnings.push_back("图标条目不是可识别的图像数据（缩略图会失败）");
        }
      }
    }
  } else {
    out->warnings.push_back("未找到 launcher 图标");
  }

  if (out->package.empty()) out->warnings.push_back("清单里没有 package 属性");
  return true;
}

bool CollectApkInfoFromSource(std::shared_ptr<ByteSource> source, bool want_sha256, ApkInfo* out,
                              Error* err) {
  (void)want_sha256;  // 哈希要按路径流式读，见 CollectApkInfo
  if (out == nullptr) return false;

  ZipReader outer;
  if (!outer.OpenSource(std::move(source), err)) return false;

  // 多 APK 容器（.xapk/.apks/.apkm）：先取出 base.apk 的数据源，再按普通 APK 解析
  PackageOpen package;
  if (!ResolvePackage(outer, &package, err)) return false;

  if (package.kind == ContainerKind::kApk) {
    if (!ScanApkZip(outer, out, err)) return false;
  } else {
    ZipReader base;
    if (!base.OpenSource(package.apk, err)) {
      if (err) err->set(L"容器里的 base.apk 打不开（中央目录损坏）");
      return false;
    }
    if (!ScanApkZip(base, out, err)) return false;

    out->container = package.kind;
    out->base_entry = package.base_entry;
    out->splits = package.splits;
  }
  out->size = outer.file_size();

  // 图标兜底：base 里没有 launcher 图标时用容器根上的 icon.png
  if (out->icon_source.empty() && !package.root_icon.empty()) {
    const ZipEntry* entry = outer.Find(package.root_icon);
    std::vector<uint8_t> bytes;
    if (entry != nullptr && outer.Read(*entry, kMaxIconRead, &bytes, nullptr)) {
      out->icon_source = package.root_icon;
      out->icon_from_container = true;
      PngSize(bytes, &out->icon_width, &out->icon_height);
      for (auto it = out->warnings.begin(); it != out->warnings.end(); ++it) {
        if (*it == "未找到 launcher 图标") {
          out->warnings.erase(it);
          break;
        }
      }
    }
  }
  return true;
}

bool CollectApkInfo(const std::wstring& file, bool want_sha256, ApkInfo* out, Error* err) {
  if (out == nullptr) return false;
  *out = ApkInfo();

  LARGE_INTEGER freq{};
  LARGE_INTEGER t0{};
  ::QueryPerformanceFrequency(&freq);
  ::QueryPerformanceCounter(&t0);

  FileStamp stamp;
  if (!GetFileStamp(file, &stamp)) {
    if (err) err->set(L"无法读取文件属性");
    return false;
  }

  auto source = OpenFileSource(file, err);
  if (!source) return false;

  // 注意：CollectApkInfoFromSource 会重置结构体，所以文件元数据在它之后再填
  ApkInfo parsed;
  if (!CollectApkInfoFromSource(source, false, &parsed, err)) return false;
  *out = std::move(parsed);

  WideToUtf8(file, &out->path);
  out->size = stamp.size;
  out->mtime_filetime = stamp.mtimeFileTime;

  if (want_sha256) {
    std::wstring hex;
    if (Sha256FileHex(file, &hex, err)) {
      WideToUtf8(hex, &out->sha256);
    } else {
      out->warnings.push_back("sha256 计算失败");
    }
  }

  LARGE_INTEGER t1{};
  ::QueryPerformanceCounter(&t1);
  out->elapsed_ms =
      static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  return true;
}

}  // namespace apksx
