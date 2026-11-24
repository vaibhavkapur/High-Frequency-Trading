#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sor {

struct JsonValue {
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage data{nullptr};

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::nullptr_t>(data);
    }
    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
    [[nodiscard]] bool is_number() const noexcept { return std::holds_alternative<double>(data); }
    [[nodiscard]] bool is_string() const noexcept {
        return std::holds_alternative<std::string>(data);
    }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(data); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(data); }

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept {
        return is_bool() ? std::get<bool>(data) : fallback;
    }
    [[nodiscard]] double as_double(double fallback = 0.0) const noexcept {
        return is_number() ? std::get<double>(data) : fallback;
    }
    [[nodiscard]] std::int64_t as_i64(std::int64_t fallback = 0) const noexcept {
        return is_number() ? static_cast<std::int64_t>(std::get<double>(data)) : fallback;
    }
    [[nodiscard]] const std::string& as_string() const { return std::get<std::string>(data); }
    [[nodiscard]] const Array& as_array() const { return std::get<Array>(data); }
    [[nodiscard]] const Object& as_object() const { return std::get<Object>(data); }

    [[nodiscard]] const JsonValue* find(const std::string& key) const noexcept {
        if (!is_object()) return nullptr;
        const auto& obj = std::get<Object>(data);
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
};

// Strict, zero-dependency JSON parser with recursion depth limit (max 64).
[[nodiscard]] bool parse_json(std::string_view input, JsonValue& out, std::string& error_out);

}  // namespace sor
