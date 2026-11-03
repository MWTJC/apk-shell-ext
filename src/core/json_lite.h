// 极简 JSON 解析（只读 DOM）：属性页要读 `apkinfo.exe --json` 的输出。
//
// 为什么不引第三方：ADR-0004 的依赖边界——进 explorer 地址空间的代码只允许系统库，
// 而 nlohmann/json 这类 header-only 会把几十 KB 模板代码塞进 DLL。
// 这里只实现 RFC 8259 的合法子集：对象 / 数组 / 字符串 / 数字 / true / false / null，
// 带深度与长度上限，解析失败返回 false 而不是抛异常（shell 扩展里不允许有异常逃逸）。
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/common.h"

namespace apksx {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;  // String 的内容（UTF-8，已解转义）；Number 的原文字面量
  std::vector<std::unique_ptr<Value>> items;                                   // Array
  std::vector<std::pair<std::string, std::unique_ptr<Value>>> members;          // Object

  const Value* Find(std::string_view key) const;
  const Value* At(size_t index) const;
  size_t size() const { return type == Type::Array ? items.size() : members.size(); }

  bool is_object() const { return type == Type::Object; }
  bool is_string() const { return type == Type::String; }
  bool is_number() const { return type == Type::Number; }
  bool is_bool() const { return type == Type::Bool; }

  std::string_view as_string() const {
    return type == Type::String ? std::string_view(text) : std::string_view();
  }
  double as_number(double fallback = 0.0) const { return type == Type::Number ? number : fallback; }
  bool as_bool(bool fallback = false) const { return type == Type::Bool ? boolean : fallback; }
};

using ValuePtr = std::unique_ptr<Value>;

// 解析整段文本；成功时 out 指向根值，失败时 err 带位置信息
bool Parse(std::string_view text, ValuePtr* out, Error* err);

}  // namespace json
}  // namespace apksx
