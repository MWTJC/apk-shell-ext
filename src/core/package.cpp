#include "core/package.h"

#include <algorithm>
#include <cctype>

#include "core/apk_icon.h"
#include "core/json_lite.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

// deflate 的 base.apk 只能整块解压；上限与 kMaxTotalUncompressed 一致
constexpr uint64_t kMaxBaseApkInflate = 512ull * 1024 * 1024;
// manifest.json（xapk）只有几 KB
constexpr uint64_t kMaxContainerJson = 1ull * 1024 * 1024;

std::string LowerAscii(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool EndsWithNoCase(const std::string& name, const char* suffix) {
  const size_t len = std::char_traits<char>::length(suffix);
  if (name.size() < len) return false;
  return LowerAscii(name.substr(name.size() - len)) == LowerAscii(suffix);
}

bool StartsWithNoCase(const std::string& name, const char* prefix) {
  const size_t len = std::char_traits<char>::length(prefix);
  if (name.size() < len) return false;
  return LowerAscii(name.substr(0, len)) == LowerAscii(prefix);
}

// xapk 的 manifest.json 里有 package_name：APKPure 的 base 文件名就叫 <package_name>.apk
bool ParseXapkPackageName(const ZipReader& outer, std::string* out) {
  const ZipEntry* entry = outer.Find("manifest.json");
  if (entry == nullptr) return false;
  std::vector<uint8_t> bytes;
  if (!outer.Read(*entry, kMaxContainerJson, &bytes, nullptr)) return false;
  json::ValuePtr doc;
  Error err;
  if (!json::Parse(std::string(bytes.begin(), bytes.end()), &doc, &err) || doc == nullptr) {
    return false;
  }
  const json::Value* name = doc->Find("package_name");
  if (name == nullptr || !name->is_string()) return false;
  *out = std::string(name->as_string());
  return !out->empty();
}

ContainerKind DetectKind(const ZipReader& outer) {
  if (outer.Find("toc.pb") != nullptr) return ContainerKind::kApks;        // bundletool
  if (outer.Find("manifest.json") != nullptr) return ContainerKind::kXapk;  // APKPure
  if (outer.Find("info.json") != nullptr) return ContainerKind::kApkm;      // APKMirror
  return ContainerKind::kSplits;
}

const ZipEntry* PickBase(const ZipReader& outer, const std::vector<const ZipEntry*>& candidates) {
  // 1) 名字就叫 base.apk（.apks / .apkm 的惯例）
  for (const ZipEntry* entry : candidates) {
    if (LowerAscii(entry->name) == "base.apk") return entry;
  }
  // 2) xapk：manifest.json 的 package_name + ".apk"（APKPure 的惯例）
  std::string package_name;
  if (ParseXapkPackageName(outer, &package_name)) {
    const std::string want = LowerAscii(package_name) + ".apk";
    for (const ZipEntry* entry : candidates) {
      if (LowerAscii(entry->name) == want) return entry;
    }
  }
  // 3) 排除明摆着的分包（config.* / split_*），剩下最大的那个当 base
  const ZipEntry* best = nullptr;
  for (const ZipEntry* entry : candidates) {
    const std::string lower = LowerAscii(entry->name);
    if (StartsWithNoCase(lower, "config.") || StartsWithNoCase(lower, "split_")) continue;
    if (best == nullptr || entry->uncompressed_size > best->uncompressed_size) best = entry;
  }
  if (best != nullptr) return best;
  // 4) 实在分不出来：取最大的
  for (const ZipEntry* entry : candidates) {
    if (best == nullptr || entry->uncompressed_size > best->uncompressed_size) best = entry;
  }
  return best;
}

bool MaterializeBase(const ZipReader& outer, const ZipEntry& base, PackageOpen* out, Error* err) {
  // Stored：直接在外层文件上开窗口（1.25 GB 的 xapk 也不会读进内存）
  if (auto slice = outer.OpenStoredEntry(base)) {
    out->apk = std::move(slice);
    return true;
  }
  if (base.uncompressed_size > kMaxBaseApkInflate) {
    if (err) err->set(L"内层 base.apk 是 deflate 且超过 512 MB，v1 不解压");
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!outer.Read(base, kMaxBaseApkInflate, &bytes, err)) return false;
  out->apk = MemorySource(std::move(bytes));
  return out->apk != nullptr;
}

}  // namespace

const char* ContainerKindName(ContainerKind kind) {
  switch (kind) {
    case ContainerKind::kApk: return "apk";
    case ContainerKind::kXapk: return "xapk";
    case ContainerKind::kApks: return "apks";
    case ContainerKind::kApkm: return "apkm";
    case ContainerKind::kSplits: return "splits";
  }
  return "unknown";
}

bool ResolvePackage(const ZipReader& outer, PackageOpen* out, Error* err) {
  if (out == nullptr || !outer.IsOpen()) return false;
  *out = PackageOpen();

  if (outer.Find("AndroidManifest.xml") != nullptr) {
    out->kind = ContainerKind::kApk;
    return true;
  }

  // 根上的 *.apk（分包容器的惯例；子目录里的 Android/obb 之类不参与）
  std::vector<const ZipEntry*> candidates;
  for (const ZipEntry& entry : outer.entries()) {
    if (entry.name.find('/') != std::string::npos) continue;
    if (!EndsWithNoCase(entry.name, ".apk")) continue;
    candidates.push_back(&entry);
  }
  if (candidates.empty()) {
    if (err) err->set(L"包内既没有 AndroidManifest.xml，也没有内层 .apk");
    return false;
  }

  out->kind = DetectKind(outer);
  const ZipEntry* base = PickBase(outer, candidates);
  if (base == nullptr) {
    if (err) err->set(L"容器里找不到 base.apk");
    return false;
  }

  out->base_entry = base->name;
  out->base_size = base->uncompressed_size;
  if (!MaterializeBase(outer, *base, out, err)) return false;

  out->splits.reserve(candidates.size());
  for (const ZipEntry* entry : candidates) {
    SplitApk split;
    split.name = entry->name;
    split.size = entry->uncompressed_size;
    split.is_base = entry == base;
    const std::string lower = LowerAscii(entry->name);
    split.kind = split.is_base ? "base"
                               : (StartsWithNoCase(lower, "config.") ? "config" : "other");
    out->splits.push_back(std::move(split));
  }

  // 容器根上的 icon.png：base 里没有 launcher 图标时的兜底（apkm/xapk 常见）
  if (const ZipEntry* icon = outer.Find("icon.png")) {
    out->root_icon = icon->name;
  } else if (const ZipEntry* icon2 = outer.Find("Icon.png")) {
    out->root_icon = icon2->name;
  }
  return true;
}

bool OpenPackageFile(const std::wstring& path, PackageOpen* out, Error* err) {
  if (out == nullptr) return false;
  *out = PackageOpen();

  auto source = OpenFileSource(path, err);
  if (!source) return false;

  ZipReader outer;
  if (!outer.OpenSource(source, err)) return false;

  PackageOpen package;
  if (!ResolvePackage(outer, &package, err)) return false;
  *out = std::move(package);
  // 单 APK：外层 ZIP 与 APK 是同一份数据，直接给文件源
  if (out->kind == ContainerKind::kApk) out->apk = std::move(source);
  return out->apk != nullptr;
}

bool ReadPackageIcon(const ZipReader& outer, PackageIcon* out, Error* err, const Deadline* deadline) {
  if (out == nullptr) return false;
  *out = PackageIcon();

  PackageOpen package;
  if (!ResolvePackage(outer, &package, err)) return false;

  ZipReader base;
  const ZipReader* apk = &outer;
  if (package.kind != ContainerKind::kApk) {
    if (!base.OpenSource(package.apk, err)) {
      if (err) err->set(L"内层 base.apk 打不开（中央目录损坏）");
      return false;
    }
    apk = &base;
  }

  IconLocation icon;
  ManifestInfo manifest;
  Error locate_error;
  if (LocateApkIconInZip(*apk, &icon, &manifest, &locate_error, deadline)) {
    const ZipEntry* entry = apk->Find(icon.entry_path);
    if (entry != nullptr && apk->Read(*entry, kMaxIconRead, &out->bytes, nullptr)) {
      out->source = package.kind == ContainerKind::kApk
                        ? icon.entry_path
                        : package.base_entry + "!" + icon.entry_path;
      out->note = icon.note;
      out->adaptive = icon.adaptive_xml;
      return true;
    }
  }

  // 兜底：容器根上的 icon.png
  if (!package.root_icon.empty()) {
    const ZipEntry* entry = outer.Find(package.root_icon);
    if (entry != nullptr && outer.Read(*entry, kMaxIconRead, &out->bytes, nullptr)) {
      out->source = package.root_icon;
      out->note = "容器根上的 icon.png（base 里没有可用图标）";
      out->from_container_root = true;
      return true;
    }
  }

  if (err) {
    err->set(locate_error.empty() ? L"包里没有可用的 launcher 图标" : locate_error.message);
  }
  return false;
}

}  // namespace apksx
