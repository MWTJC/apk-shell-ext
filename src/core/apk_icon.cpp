#include "core/apk_icon.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/arsc.h"
#include "core/axml.h"
#include "core/log.h"
#include "core/zip_reader.h"

namespace apksx {
namespace {

constexpr const char* kManifestEntry = "AndroidManifest.xml";
constexpr const char* kArscEntry = "resources.arsc";

double QpcToMs(long long ticks) {
  static const double freq = [] {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  return static_cast<double>(ticks) * 1000.0 / freq;
}

long long QpcNow() {
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return t.QuadPart;
}

std::string Lower(std::string_view in) {
  std::string out(in);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool IsRasterIconName(std::string_view lower) {
  return EndsWith(lower, ".png") || EndsWith(lower, ".webp") || EndsWith(lower, ".jpg") ||
         EndsWith(lower, ".jpeg");
}

// 从路径里读密度：-xxxhdpi-v4 / _xxxhdpi / anydpi
int DensityRankFromPath(std::string_view path) {
  const std::string lower = Lower(path);
  static const struct {
    const char* token;
    int rank;
  } kTable[] = {
      {"xxxhdpi", 640}, {"xxhdpi", 480}, {"xhdpi", 320}, {"hdpi", 240},
      {"mdpi", 160},    {"ldpi", 120},   {"nodpi", 1},   {"anydpi", 0},
  };
  for (const auto& t : kTable) {
    if (lower.find(t.token) != std::string::npos) return t.rank;
  }
  return 2;  // 无密度限定：当作比 nodpi 好、比 mdpi 差
}

uint16_t DensityFromRank(int rank) {
  if (rank <= 0) return kDensityAny;
  if (rank == 1) return kDensityNone;
  if (rank == 2) return kDensityDefault;
  return static_cast<uint16_t>(rank);
}

std::string Basename(std::string_view path) {
  const size_t pos = path.find_last_of('/');
  return std::string(pos == std::string_view::npos ? path : path.substr(pos + 1));
}

// 路径是否在"图标可能出现的目录"里（docs/formats.md §8 白名单的子集）
bool IsPlausibleResPath(std::string_view path) {
  return path.rfind("res/", 0) == 0 || path.rfind("base/res/", 0) == 0 ||
         path.rfind("assets/", 0) == 0;
}

// 在 entries 里按名字找（精确 → 大小写不敏感 → 仅 basename）
const ZipEntry* FindEntryLoose(const ZipReader& zip, std::string_view path) {
  if (const ZipEntry* e = zip.Find(path)) return e;
  const std::string want = Lower(path);
  for (const ZipEntry& e : zip.entries()) {
    if (Lower(e.name) == want) return &e;
  }
  const std::string want_base = Lower(Basename(path));
  for (const ZipEntry& e : zip.entries()) {
    if (Lower(Basename(e.name)) == want_base) return &e;
  }
  return nullptr;
}

}  // namespace

bool ParseManifestElements(const std::vector<AxmlElement>& elements, ManifestInfo* out,
                           Error* err) {
  if (out == nullptr) return false;
  *out = ManifestInfo();

  const AxmlElement* application = nullptr;
  for (const AxmlElement& el : elements) {
    if (el.tag == "manifest") {
      GetStringAttr(el, 0, "package", &out->package);
      GetIntAttr(el, kAttrVersionCode, "versionCode", &out->version_code);
      GetStringAttr(el, kAttrVersionName, "versionName", &out->version_name);
      GetStringAttr(el, kAttrLabel, "label", &out->label);
    } else if (el.tag == "uses-sdk") {
      GetIntAttr(el, kAttrMinSdkVersion, "minSdkVersion", &out->min_sdk);
      GetIntAttr(el, kAttrTargetSdkVersion, "targetSdkVersion", &out->target_sdk);
    } else if (el.tag == "application" && application == nullptr) {
      application = &el;
    }
  }

  if (application == nullptr) {
    if (err) err->set(L"清单里没有 <application>");
    return false;
  }

  if (out->label.empty()) {
    GetStringAttr(*application, kAttrLabel, "label", &out->label);
  }

  if (const AxmlAttr* icon = application->Find(kAttrIcon, "icon")) {
    out->has_icon = true;
    if (icon->type == kResTypeReference && icon->data != 0) {
      out->icon_res_id = icon->data;
    } else if (icon->has_string) {
      out->icon_literal_path = icon->string_value;
    }
  }
  return true;
}

namespace {

bool ParseManifestBytes(const std::vector<uint8_t>& bytes, ManifestInfo* out, Error* err) {
  std::vector<AxmlElement> elements;
  if (!ParseAxml(bytes, &elements, err)) return false;
  return ParseManifestElements(elements, out, err);
}

}  // namespace

double Deadline::elapsed_ms() const { return QpcToMs(Now() - start_); }
long long Deadline::Now() { return QpcNow(); }

bool ReadManifestFromZip(const ZipReader& zip, ManifestInfo* manifest, Error* err,
                         const Deadline* deadline) {
  if (manifest == nullptr) return false;
  *manifest = ManifestInfo();

  const ZipEntry* entry = zip.Find(kManifestEntry);
  if (entry == nullptr) {
    if (err) err->set(L"包内缺少 AndroidManifest.xml（不是有效 APK）");
    return false;
  }

  std::vector<uint8_t> bytes;
  if (!zip.Read(*entry, kMaxManifestRead, &bytes, err)) return false;
  if (deadline != nullptr && deadline->expired()) {
    if (err) err->set(L"读取清单超时");
    return false;
  }
  return ParseManifestBytes(bytes, manifest, err);
}

bool ReadApkManifest(const std::wstring& file, ManifestInfo* manifest, Error* err,
                     const Deadline* deadline) {
  ZipReader zip;
  if (!zip.Open(file, err)) return false;
  return ReadManifestFromZip(zip, manifest, err, deadline);
}

bool LocateIconInZip(const ZipReader& zip, const ManifestInfo& info, IconLocation* icon,
                     Error* err, const Deadline* deadline) {
  if (icon == nullptr) return false;

  if (!info.has_icon) {
    if (err) err->set(L"应用未声明 android:icon");
    return false;
  }

  // --- 1) android:icon 直接写路径
  if (!info.icon_literal_path.empty()) {
    const ZipEntry* entry = FindEntryLoose(zip, info.icon_literal_path);
    if (entry != nullptr) {
      icon->entry_path = entry->name;
      icon->note = "literal";
      return true;
    }
    DebugLogf(L"literal icon path not found: %hs", info.icon_literal_path.c_str());
  }

  // --- 2) 资源 ID → resources.arsc
  std::vector<ArscCandidate> resolved;
  if (info.icon_res_id != kInvalidResId) {
    const ZipEntry* arsc_entry = zip.Find(kArscEntry);
    if (arsc_entry != nullptr && arsc_entry->uncompressed_size <= kMaxArscRead) {
      std::vector<uint8_t> arsc_bytes;
      if (zip.Read(*arsc_entry, kMaxArscRead, &arsc_bytes, nullptr)) {
        ArscTable table;
        if (table.Parse(arsc_bytes, nullptr)) {
          uint32_t pending_id = info.icon_res_id;
          for (int depth = 0; depth < 4 && pending_id != kInvalidResId; ++depth) {
            std::vector<ArscCandidate> cands;
            table.CollectCandidates(pending_id, &cands);

            // 跟随一层引用（如 @color / @drawable → 另一个资源）
            bool followed = false;
            for (const ArscCandidate& c : cands) {
              if (c.value_type == kResTypeReference && !c.is_bag && c.value_data != 0) {
                pending_id = c.value_data;
                followed = true;
                break;
              }
            }
            if (followed) continue;

            for (ArscCandidate& c : cands) {
              if (c.value_type != kResTypeString || c.value_string.empty()) continue;
              resolved.push_back(std::move(c));
            }
            break;
          }
        }
        if (deadline != nullptr && deadline->expired()) {
          if (err) err->set(L"解析资源表超时");
          return false;
        }
      }
    }
  }

  // --- 3) 从候选里挑最合适的：光栅优先，密度最高优先，mipmap 优先于 drawable
  std::sort(resolved.begin(), resolved.end(), [](const ArscCandidate& a, const ArscCandidate& b) {
    const bool a_raster = IsRasterIconName(Lower(a.value_string));
    const bool b_raster = IsRasterIconName(Lower(b.value_string));
    if (a_raster != b_raster) return a_raster;  // 光栅在前
    const bool a_mipmap = Lower(a.value_string).find("/mipmap") != std::string::npos;
    const bool b_mipmap = Lower(b.value_string).find("/mipmap") != std::string::npos;
    if (a_mipmap != b_mipmap) return a_mipmap;
    return a.density > b.density;
  });

  for (const ArscCandidate& c : resolved) {
    const std::string& path = c.value_string;
    if (!IsPlausibleResPath(path)) continue;
    const ZipEntry* entry = FindEntryLoose(zip, path);
    if (entry == nullptr) continue;
    icon->entry_path = entry->name;
    icon->density = c.density;
    icon->adaptive_xml = EndsWith(Lower(entry->name), ".xml");
    icon->note = "arsc";
    if (!icon->adaptive_xml) return true;
    break;  // 命中 XML：先记下，后面找光栅回退
  }

  // --- 4) 找到的是 anydpi 的 XML（adaptive icon）：M1 用同名光栅回退
  if (!icon->entry_path.empty() && icon->adaptive_xml) {
    const std::string stem = Basename(icon->entry_path);
    const std::string base = stem.substr(0, stem.find_last_of('.'));
    const ZipEntry* best = nullptr;
    int best_rank = -1;
    for (const ZipEntry& e : zip.entries()) {
      if (!IsRasterIconName(Lower(e.name))) continue;
      const std::string name = Lower(Basename(e.name));
      if (name.rfind(base, 0) != 0) continue;
      const int rank = DensityRankFromPath(e.name);
      if (rank > best_rank) {
        best_rank = rank;
        best = &e;
      }
    }
    if (best != nullptr) {
      icon->entry_path = best->name;
      icon->density = DensityFromRank(best_rank);
      icon->adaptive_xml = false;
      icon->note = "adaptive→raster fallback";
      return true;
    }
    if (err) err->set(L"只有 adaptive icon（M1 不支持合成）");
    return false;
  }

  // --- 5) 启发式：按文件名在 res/ 下猜（arsc 损坏/缺失/资源丢失时的兜底）
  {
    const ZipEntry* best = nullptr;
    int best_rank = -1;
    for (const ZipEntry& e : zip.entries()) {
      if (!IsPlausibleResPath(e.name) || !IsRasterIconName(Lower(e.name))) continue;
      const std::string name = Lower(Basename(e.name));
      if (name.rfind("ic_launcher", 0) != 0 && name.rfind("app_icon", 0) != 0 &&
          name.rfind("icon", 0) != 0) {
        continue;
      }
      const int rank = DensityRankFromPath(e.name);
      const bool mipmap = Lower(e.name).find("/mipmap") != std::string::npos;
      const int score = (mipmap ? 10000 : 0) + rank;
      if (score > best_rank) {
        best_rank = score;
        best = &e;
      }
    }
    if (best != nullptr) {
      icon->entry_path = best->name;
      icon->density = DensityFromRank(best_rank % 10000);
      icon->heuristic = true;
      icon->note = "heuristic";
      return true;
    }
  }

  if (err) err->set(L"未找到可用的 launcher 图标");
  return false;
}

bool LocateApkIconInZip(const ZipReader& zip, IconLocation* icon, ManifestInfo* manifest,
                        Error* err, const Deadline* deadline) {
  if (icon == nullptr) return false;
  *icon = IconLocation();

  ManifestInfo info;
  if (!ReadManifestFromZip(zip, &info, err, deadline)) return false;
  if (manifest != nullptr) *manifest = info;
  return LocateIconInZip(zip, info, icon, err, deadline);
}

bool LocateApkIcon(const std::wstring& file, IconLocation* icon, ManifestInfo* manifest,
                   Error* err, const Deadline* deadline) {
  ZipReader zip;
  if (!zip.Open(file, err)) return false;
  return LocateApkIconInZip(zip, icon, manifest, err, deadline);
}

}  // namespace apksx
