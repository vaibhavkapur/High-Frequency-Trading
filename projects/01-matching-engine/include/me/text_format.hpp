// Human-readable command and report formats used by the CLI, examples and tests.
//
// Command lines (whitespace separated, '#' starts a comment):
//   N <symbol> <order_id> <B|S> <price> <qty>          new limit order
//   C <symbol> <order_id>                              cancel
//   R <symbol> <order_id> <new_remaining_qty>          reduce
//   X <symbol> <order_id> <new_order_id> <price> <qty> cancel/replace
#pragma once

#include <string>
#include <string_view>

#include "me/protocol.hpp"

namespace me {

enum class ParseStatus { Ok, Blank, Error };

// Parses one line. On Error, *error (if given) describes the problem.
ParseStatus parse_command(std::string_view line, Command& out, std::string* error = nullptr);

std::string format_command(const Command& c);
std::string format_report(const Report& r);
std::string_view to_string(RejectReason reason) noexcept;

}  // namespace me
