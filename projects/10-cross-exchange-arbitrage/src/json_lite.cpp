#include "xarb/json_lite.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace xarb {

namespace {

class Parser {
 public:
  Parser(std::string_view s, size_t max_depth) : s_(s), max_depth_(max_depth) {}

  JsonParseResult parse() {
    skip_ws();
    auto val = parse_value(0);
    if (!val) return {std::nullopt, err_};
    skip_ws();
    if (pos_ != s_.size()) {
      return {std::nullopt, "trailing characters after JSON root"};
    }
    return {std::move(val), {}};
  }

 private:
  void skip_ws() noexcept {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  std::optional<JsonValue> parse_value(size_t depth) {
    if (depth > max_depth_) {
      err_ = "max JSON nesting depth exceeded";
      return std::nullopt;
    }
    skip_ws();
    if (pos_ >= s_.size()) {
      err_ = "unexpected end of JSON input";
      return std::nullopt;
    }
    char c = s_[pos_];
    if (c == '{') return parse_object(depth + 1);
    if (c == '[') return parse_array(depth + 1);
    if (c == '"') {
      auto str = parse_string();
      if (!str) return std::nullopt;
      JsonValue v;
      v.data = std::move(*str);
      return v;
    }
    if (c == 't') return parse_literal("true", true);
    if (c == 'f') return parse_literal("false", false);
    if (c == 'n') return parse_null();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    err_ = "unexpected token in JSON";
    return std::nullopt;
  }

  std::optional<JsonValue> parse_null() {
    if (s_.substr(pos_, 4) == "null") {
      pos_ += 4;
      return JsonValue{nullptr};
    }
    err_ = "invalid null literal";
    return std::nullopt;
  }

  std::optional<JsonValue> parse_literal(std::string_view lit, bool val) {
    if (s_.substr(pos_, lit.size()) == lit) {
      pos_ += lit.size();
      JsonValue v;
      v.data = val;
      return v;
    }
    err_ = "invalid boolean literal";
    return std::nullopt;
  }

  std::optional<std::string> parse_string() {
    if (pos_ >= s_.size() || s_[pos_] != '"') {
      err_ = "expected '\"'";
      return std::nullopt;
    }
    ++pos_;
    std::string out;
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20U) {
        err_ = "unescaped control character in JSON string";
        return std::nullopt;
      }
      if (c == '\\') {
        if (pos_ >= s_.size()) {
          err_ = "unterminated escape sequence";
          return std::nullopt;
        }
        char esc = s_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > s_.size()) {
              err_ = "truncated \\u escape";
              return std::nullopt;
            }
            uint32_t cp = 0;
            for (int k = 0; k < 4; ++k) {
              char h = s_[pos_++];
              cp <<= 4U;
              if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
              else {
                err_ = "invalid hex digit in \\u escape";
                return std::nullopt;
              }
            }
            if (cp < 0x80U) {
              out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800U) {
              out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
              out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else {
              out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
              out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
              out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            }
            break;
          }
          default:
            err_ = "unknown escape sequence";
            return std::nullopt;
        }
      } else {
        out.push_back(c);
      }
    }
    err_ = "unterminated JSON string";
    return std::nullopt;
  }

  std::optional<JsonValue> parse_number() {
    size_t start = pos_;
    if (s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
      err_ = "invalid JSON number";
      return std::nullopt;
    }
    if (s_[pos_] == '0') {
      ++pos_;
    } else {
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    }

    bool is_float = false;
    if (pos_ < s_.size() && s_[pos_] == '.') {
      is_float = true;
      ++pos_;
      if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
        err_ = "invalid fraction in JSON number";
        return std::nullopt;
      }
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    }

    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') {
        err_ = "invalid exponent in JSON number";
        return std::nullopt;
      }
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    }

    std::string_view token = s_.substr(start, pos_ - start);
    if (!is_float) {
      int64_t ival = 0;
      auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), ival);
      if (ec == std::errc() && ptr == token.data() + token.size()) {
        JsonValue v;
        v.data = ival;
        return v;
      }
    }

    std::string tmp(token);
    char* end = nullptr;
    double dval = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || !std::isfinite(dval)) {
      err_ = "JSON number out of range";
      return std::nullopt;
    }
    JsonValue v;
    v.data = dval;
    return v;
  }

  std::optional<JsonValue> parse_array(size_t depth) {
    ++pos_;  // '['
    JsonValue::Array arr;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      JsonValue v;
      v.data = std::move(arr);
      return v;
    }
    while (pos_ < s_.size()) {
      auto elem = parse_value(depth);
      if (!elem) return std::nullopt;
      arr.push_back(std::move(*elem));
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        JsonValue v;
        v.data = std::move(arr);
        return v;
      }
      err_ = "expected ',' or ']' in JSON array";
      return std::nullopt;
    }
    err_ = "unterminated JSON array";
    return std::nullopt;
  }

  std::optional<JsonValue> parse_object(size_t depth) {
    ++pos_;  // '{'
    JsonValue::Object obj;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      JsonValue v;
      v.data = std::move(obj);
      return v;
    }
    while (pos_ < s_.size()) {
      skip_ws();
      auto key = parse_string();
      if (!key) return std::nullopt;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        err_ = "expected ':' in JSON object";
        return std::nullopt;
      }
      ++pos_;
      auto val = parse_value(depth);
      if (!val) return std::nullopt;
      obj.insert_or_assign(std::move(*key), std::move(*val));
      skip_ws();
      if (pos_ < s_.size() && s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        JsonValue v;
        v.data = std::move(obj);
        return v;
      }
      err_ = "expected ',' or '}' in JSON object";
      return std::nullopt;
    }
    err_ = "unterminated JSON object";
    return std::nullopt;
  }

  std::string_view s_;
  size_t pos_{0};
  size_t max_depth_{32};
  std::string err_;
};

}  // namespace

JsonParseResult parse_json(std::string_view input, size_t max_depth, size_t max_bytes) {
  if (input.size() > max_bytes) {
    return {std::nullopt, "JSON payload exceeds max_bytes limit"};
  }
  Parser p(input, max_depth);
  return p.parse();
}

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

}  // namespace xarb
