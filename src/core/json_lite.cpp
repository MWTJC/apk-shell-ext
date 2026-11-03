#include "core/json_lite.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace apksx {
namespace json {
namespace {

constexpr int kMaxDepth = 32;
constexpr size_t kMaxStringBytes = 4u * 1024 * 1024;

class Parser {
 public:
  Parser(std::string_view text) : text_(text) {}

  bool ParseDocument(ValuePtr* out, Error* err) {
    ValuePtr root;
    SkipSpace();
    if (!ParseValue(&root, 0)) {
      Fail(err);
      return false;
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      error_ = "JSON 末尾有多余内容";
      Fail(err);
      return false;
    }
    *out = std::move(root);
    return true;
  }

 private:
  bool Fail(Error* err) {
    if (err != nullptr) {
      wchar_t buf[160];
      std::wstring message;
      if (Utf8ToWide(error_.empty() ? "JSON 解析失败" : error_, &message)) {
        _snwprintf_s(buf, _TRUNCATE, L"%s（偏移 %zu）", message.c_str(), pos_);
        err->set(buf);
      } else {
        err->set(L"JSON 解析失败");
      }
    }
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Eat(char expected) {
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ParseValue(ValuePtr* out, int depth) {
    if (depth > kMaxDepth) {
      error_ = "JSON 嵌套过深";
      return false;
    }
    if (pos_ >= text_.size()) {
      error_ = "JSON 意外结束";
      return false;
    }
    switch (text_[pos_]) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"': {
        auto value = std::make_unique<Value>();
        value->type = Type::String;
        if (!ParseString(&value->text)) return false;
        *out = std::move(value);
        return true;
      }
      case 't':
        if (text_.compare(pos_, 4, "true") == 0) {
          pos_ += 4;
          auto value = std::make_unique<Value>();
          value->type = Type::Bool;
          value->boolean = true;
          *out = std::move(value);
          return true;
        }
        error_ = "非法的字面量";
        return false;
      case 'f':
        if (text_.compare(pos_, 5, "false") == 0) {
          pos_ += 5;
          auto value = std::make_unique<Value>();
          value->type = Type::Bool;
          value->boolean = false;
          *out = std::move(value);
          return true;
        }
        error_ = "非法的字面量";
        return false;
      case 'n':
        if (text_.compare(pos_, 4, "null") == 0) {
          pos_ += 4;
          auto value = std::make_unique<Value>();
          value->type = Type::Null;
          *out = std::move(value);
          return true;
        }
        error_ = "非法的字面量";
        return false;
      default: return ParseNumber(out);
    }
  }

  bool ParseObject(ValuePtr* out, int depth) {
    if (!Eat('{')) return false;
    auto value = std::make_unique<Value>();
    value->type = Type::Object;
    SkipSpace();
    if (Eat('}')) {
      *out = std::move(value);
      return true;
    }
    while (true) {
      SkipSpace();
      std::string key;
      if (!ParseString(&key)) return false;
      SkipSpace();
      if (!Eat(':')) {
        error_ = "对象成员缺少冒号";
        return false;
      }
      SkipSpace();
      ValuePtr member;
      if (!ParseValue(&member, depth + 1)) return false;
      value->members.emplace_back(std::move(key), std::move(member));
      SkipSpace();
      if (Eat(',')) continue;
      if (Eat('}')) break;
      error_ = "对象未正确结束";
      return false;
    }
    *out = std::move(value);
    return true;
  }

  bool ParseArray(ValuePtr* out, int depth) {
    if (!Eat('[')) return false;
    auto value = std::make_unique<Value>();
    value->type = Type::Array;
    SkipSpace();
    if (Eat(']')) {
      *out = std::move(value);
      return true;
    }
    while (true) {
      SkipSpace();
      ValuePtr item;
      if (!ParseValue(&item, depth + 1)) return false;
      value->items.push_back(std::move(item));
      SkipSpace();
      if (Eat(',')) continue;
      if (Eat(']')) break;
      error_ = "数组未正确结束";
      return false;
    }
    *out = std::move(value);
    return true;
  }

  // \uXXXX → UTF-8（含代理对）
  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseHex4(uint32_t* out) {
    if (pos_ + 4 > text_.size()) {
      error_ = "\\u 转义不完整";
      return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + static_cast<size_t>(i)];
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<uint32_t>(c - 'A' + 10);
      } else {
        error_ = "\\u 转义含非十六进制字符";
        return false;
      }
      value = (value << 4) | digit;
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  bool ParseString(std::string* out) {
    if (!Eat('"')) {
      error_ = "期望字符串";
      return false;
    }
    out->clear();
    while (true) {
      if (pos_ >= text_.size()) {
        error_ = "字符串未结束";
        return false;
      }
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) {
        error_ = "字符串里出现未转义的控制字符";
        return false;
      }
      if (c != '\\') {
        out->push_back(c);
        if (out->size() > kMaxStringBytes) {
          error_ = "字符串过长";
          return false;
        }
        continue;
      }
      if (pos_ >= text_.size()) {
        error_ = "转义不完整";
        return false;
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!ParseHex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 代理对：必须紧跟 \uDC00..\uDFFF，否则按替换字符处理
            if (pos_ + 2 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              pos_ += 2;
              uint32_t low = 0;
              if (!ParseHex4(&low)) return false;
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              } else {
                cp = 0xFFFD;
              }
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          error_ = "未知的转义序列";
          return false;
      }
      if (out->size() > kMaxStringBytes) {
        error_ = "字符串过长";
        return false;
      }
    }
  }

  bool ParseNumber(ValuePtr* out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    bool digits = false;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
      digits = true;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        digits = true;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (!digits) {
      error_ = "非法的值";
      return false;
    }
    const std::string literal(text_.substr(start, pos_ - start));
    auto value = std::make_unique<Value>();
    value->type = Type::Number;
    value->text = literal;
    value->number = std::strtod(literal.c_str(), nullptr);
    *out = std::move(value);
    return true;
  }

  std::string_view text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

const Value* Value::Find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& member : members) {
    if (member.first.size() == key.size() &&
        std::memcmp(member.first.data(), key.data(), key.size()) == 0) {
      return member.second.get();
    }
  }
  return nullptr;
}

const Value* Value::At(size_t index) const {
  if (type != Type::Array || index >= items.size()) return nullptr;
  return items[index].get();
}

bool Parse(std::string_view text, ValuePtr* out, Error* err) {
  if (out == nullptr) return false;
  *out = nullptr;
  Parser parser(text);
  return parser.ParseDocument(out, err);
}

}  // namespace json
}  // namespace apksx
