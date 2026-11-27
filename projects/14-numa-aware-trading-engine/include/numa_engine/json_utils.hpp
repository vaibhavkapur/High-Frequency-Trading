#pragma once

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace numa_engine::json {

[[nodiscard]] inline std::string escape(std::string_view input) {
    std::ostringstream oss;
    for (char raw_c : input) {
        const auto c = static_cast<unsigned char>(raw_c);
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    oss << static_cast<char>(c);
                }
                break;
        }
    }
    return oss.str();
}

[[nodiscard]] inline std::string quote(std::string_view input) {
    return "\"" + escape(input) + "\"";
}

[[nodiscard]] inline std::string format_double(double v, int precision = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

}  // namespace numa_engine::json
