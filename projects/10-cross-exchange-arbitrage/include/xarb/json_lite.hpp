#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xarb {

struct JsonValue {
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;
  using Storage = std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object>;

  Storage data{nullptr};

  bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(data); }
  bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
  bool is_int() const noexcept { return std::holds_alternative<int64_t>(data); }
  bool is_double() const noexcept { return std::holds_alternative<double>(data); }
  bool is_number() const noexcept { return is_int() || is_double(); }
  bool is_string() const noexcept { return std::holds_alternative<std::string>(data); }
  bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
  bool is_object() const noexcept { return std::holds_alternative<Object>(data); }

  const std::string* as_string() const noexcept { return std::get_if<std::string>(&data); }
  const Array* as_array() const noexcept { return std::get_if<Array>(&data); }
  const Object* as_object() const noexcept { return std::get_if<Object>(&data); }

  std::optional<int64_t> as_i64() const noexcept {
    if (const auto* v = std::get_if<int64_t>(&data)) return *v;
    return std::nullopt;
  }

  std::optional<uint64_t> as_u64() const noexcept {
    if (const auto* v = std::get_if<int64_t>(&data)) {
      if (*v >= 0) return static_cast<uint64_t>(*v);
    }
    return std::nullopt;
  }

  std::optional<double> as_double() const noexcept {
    if (const auto* v = std::get_if<double>(&data)) return *v;
    if (const auto* v = std::get_if<int64_t>(&data)) return static_cast<double>(*v);
    return std::nullopt;
  }

  const JsonValue* get(std::string_view key) const noexcept {
    const auto* obj = as_object();
    if (!obj) return nullptr;
    auto it = obj->find(key);
    return it == obj->end() ? nullptr : &it->second;
  }
};

struct JsonParseResult {
  std::optional<JsonValue> value;
  std::string error;
  bool ok() const noexcept { return value.has_value(); }
};

// Fuzz-hardened JSON parser with strict depth & input-size bounds.
JsonParseResult parse_json(std::string_view input, size_t max_depth = 32, size_t max_bytes = 4 * 1024 * 1024);

// Minimal JSON string escaper for audit/record serialization.
std::string json_escape(std::string_view s);

}  // namespace xarb
