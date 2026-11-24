#include "sor/json_util.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace sor {

namespace {

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src) {}

    bool parse(JsonValue& out, std::string& err) {
        skip_ws();
        if (!parse_value(out, 0)) {
            err = err_;
            return false;
        }
        skip_ws();
        if (pos_ != src_.size()) {
            err = "trailing characters after JSON root value";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip_ws() noexcept {
        while (pos_ < src_.size() &&
               (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n' ||
                src_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool fail(const std::string& msg) {
        if (err_.empty()) {
            std::ostringstream oss;
            oss << "JSON parse error at offset " << pos_ << ": " << msg;
            err_ = oss.str();
        }
        return false;
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return fail("maximum nesting depth exceeded");
        skip_ws();
        if (pos_ >= src_.size()) return fail("unexpected end of input");

        const char c = src_[pos_];
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '"') {
            std::string s;
            if (!parse_string(s)) return false;
            out.data = std::move(s);
            return true;
        }
        if (c == 't') return parse_literal("true", JsonValue{true}, out);
        if (c == 'f') return parse_literal("false", JsonValue{false}, out);
        if (c == 'n') return parse_literal("null", JsonValue{nullptr}, out);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(out);
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    bool parse_literal(std::string_view lit, JsonValue val, JsonValue& out) {
        if (src_.substr(pos_, lit.size()) != lit) {
            return fail("expected literal " + std::string(lit));
        }
        pos_ += lit.size();
        out = std::move(val);
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return fail("expected '\"'");
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20U) {
                return fail("unescaped control character in string");
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) return fail("unterminated escape sequence");
                const char esc = src_[pos_++];
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
                        if (pos_ + 4 > src_.size()) return fail("incomplete unicode escape");
                        unsigned int code = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = src_[pos_++];
                            code <<= 4U;
                            if (h >= '0' && h <= '9') code |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f')
                                code |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')
                                code |= static_cast<unsigned int>(h - 'A' + 10);
                            else
                                return fail("invalid hex digit in unicode escape");
                        }
                        if (code <= 0x7FU) {
                            out.push_back(static_cast<char>(code));
                        } else {
                            out.push_back('?');
                        }
                        break;
                    }
                    default: return fail("invalid escape character");
                }
            } else {
                out.push_back(c);
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(JsonValue& out) {
        const std::size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            return fail("invalid number");
        }
        if (src_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                return fail("invalid fractional number");
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                return fail("invalid exponent in number");
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }

        const std::string token(src_.substr(start, pos_ - start));
        char* end_ptr = nullptr;
        const double val = std::strtod(token.c_str(), &end_ptr);
        if (end_ptr != token.c_str() + token.size() || !std::isfinite(val)) {
            return fail("number out of range");
        }
        out.data = val;
        return true;
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_;  // skip '['
        JsonValue::Array arr;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            out.data = std::move(arr);
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parse_value(elem, depth)) return false;
            arr.push_back(std::move(elem));
            skip_ws();
            if (pos_ >= src_.size()) return fail("unterminated array");
            if (src_[pos_] == ']') {
                ++pos_;
                out.data = std::move(arr);
                return true;
            }
            if (src_[pos_] != ',') return fail("expected ',' or ']' in array");
            ++pos_;
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_;  // skip '{'
        JsonValue::Object obj;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            out.data = std::move(obj);
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') return fail("expected ':' after key");
            ++pos_;
            JsonValue val;
            if (!parse_value(val, depth)) return false;
            obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ >= src_.size()) return fail("unterminated object");
            if (src_[pos_] == '}') {
                ++pos_;
                out.data = std::move(obj);
                return true;
            }
            if (src_[pos_] != ',') return fail("expected ',' or '}' in object");
            ++pos_;
        }
    }

    std::string_view src_;
    std::size_t pos_{0};
    std::string err_{};
};

}  // namespace

bool parse_json(std::string_view input, JsonValue& out, std::string& error_out) {
    Parser p(input);
    return p.parse(out, error_out);
}

}  // namespace sor
