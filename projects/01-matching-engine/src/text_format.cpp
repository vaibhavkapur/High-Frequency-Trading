#include "me/text_format.hpp"

#include <charconv>
#include <vector>

namespace me {
namespace {

std::vector<std::string_view> tokenize(std::string_view line) {
  std::vector<std::string_view> tokens;
  if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') ++i;
    if (i > start) tokens.push_back(line.substr(start, i - start));
  }
  return tokens;
}

template <class T>
bool parse_int(std::string_view token, T& out) {
  const char* const end = token.data() + token.size();
  const auto [ptr, ec] = std::from_chars(token.data(), end, out);
  return ec == std::errc{} && ptr == end;
}

ParseStatus fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return ParseStatus::Error;
}

char side_char(Side s) {
  return s == Side::Buy ? 'B' : s == Side::Sell ? 'S' : '?';
}

}  // namespace

ParseStatus parse_command(std::string_view line, Command& out, std::string* error) {
  const std::vector<std::string_view> t = tokenize(line);
  if (t.empty()) return ParseStatus::Blank;
  if (t[0].size() != 1) return fail(error, "unknown command '" + std::string(t[0]) + "'");

  Command c;
  std::size_t expected = 0;
  switch (t[0][0]) {
    case 'N':
      c.type = CommandType::New;
      expected = 6;
      break;
    case 'C':
      c.type = CommandType::Cancel;
      expected = 3;
      break;
    case 'R':
      c.type = CommandType::Reduce;
      expected = 4;
      break;
    case 'X':
      c.type = CommandType::Replace;
      expected = 6;
      break;
    default:
      return fail(error, "unknown command '" + std::string(t[0]) + "'");
  }
  if (t.size() != expected) {
    return fail(error, "expected " + std::to_string(expected) + " fields, got " + std::to_string(t.size()));
  }
  if (!parse_int(t[1], c.symbol)) return fail(error, "bad symbol");
  if (!parse_int(t[2], c.order_id)) return fail(error, "bad order id");

  switch (c.type) {
    case CommandType::New:
      if (t[3] == "B") {
        c.side = Side::Buy;
      } else if (t[3] == "S") {
        c.side = Side::Sell;
      } else {
        return fail(error, "side must be B or S");
      }
      if (!parse_int(t[4], c.price)) return fail(error, "bad price");
      if (!parse_int(t[5], c.qty)) return fail(error, "bad quantity");
      break;
    case CommandType::Cancel:
      break;
    case CommandType::Reduce:
      if (!parse_int(t[3], c.qty)) return fail(error, "bad quantity");
      break;
    case CommandType::Replace:
      if (!parse_int(t[3], c.new_order_id)) return fail(error, "bad new order id");
      if (!parse_int(t[4], c.price)) return fail(error, "bad price");
      if (!parse_int(t[5], c.qty)) return fail(error, "bad quantity");
      break;
  }
  out = c;
  return ParseStatus::Ok;
}

std::string format_command(const Command& c) {
  const std::string head = std::to_string(c.symbol) + " " + std::to_string(c.order_id);
  switch (c.type) {
    case CommandType::New:
      return "N " + head + " " + side_char(c.side) + " " + std::to_string(c.price) + " " + std::to_string(c.qty);
    case CommandType::Cancel:
      return "C " + head;
    case CommandType::Reduce:
      return "R " + head + " " + std::to_string(c.qty);
    case CommandType::Replace:
      return "X " + head + " " + std::to_string(c.new_order_id) + " " + std::to_string(c.price) + " " +
             std::to_string(c.qty);
  }
  return "? type=" + std::to_string(static_cast<unsigned>(c.type)) + " " + head;
}

std::string_view to_string(RejectReason reason) noexcept {
  switch (reason) {
    case RejectReason::None:
      return "None";
    case RejectReason::UnknownCommand:
      return "UnknownCommand";
    case RejectReason::UnknownSymbol:
      return "UnknownSymbol";
    case RejectReason::InvalidSide:
      return "InvalidSide";
    case RejectReason::InvalidPrice:
      return "InvalidPrice";
    case RejectReason::InvalidTick:
      return "InvalidTick";
    case RejectReason::InvalidQuantity:
      return "InvalidQuantity";
    case RejectReason::DuplicateOrderId:
      return "DuplicateOrderId";
    case RejectReason::UnknownOrderId:
      return "UnknownOrderId";
    case RejectReason::BookFull:
      return "BookFull";
  }
  return "Unknown";
}

std::string format_report(const Report& r) {
  std::string s = "seq=" + std::to_string(r.seq) + " sym=" + std::to_string(r.symbol) + " ";
  const auto field = [&s](const char* name, auto value) {
    s += ' ';
    s += name;
    s += '=';
    s += std::to_string(value);
  };
  const auto side = [&s, &r] {
    s += " side=";
    s += side_char(r.side);
  };
  switch (r.type) {
    case ReportType::Accepted:
      s += "ACCEPTED";
      field("id", r.order_id);
      side();
      field("px", r.price);
      field("qty", r.qty);
      return s;
    case ReportType::Rejected:
      s += "REJECTED";
      field("id", r.order_id);
      s += " reason=";
      s += to_string(r.reason);
      return s;
    case ReportType::Trade:
      s += "TRADE";
      field("taker", r.order_id);
      field("maker", r.other_id);
      side();
      field("px", r.price);
      field("qty", r.qty);
      field("taker_leaves", r.leaves);
      field("maker_leaves", r.other_leaves);
      return s;
    case ReportType::Canceled:
      s += "CANCELED";
      field("id", r.order_id);
      side();
      field("px", r.price);
      field("qty", r.qty);
      return s;
    case ReportType::Reduced:
      s += "REDUCED";
      field("id", r.order_id);
      side();
      field("px", r.price);
      field("removed", r.qty);
      field("leaves", r.leaves);
      return s;
    case ReportType::Replaced:
      s += "REPLACED";
      field("id", r.order_id);
      field("new_id", r.other_id);
      side();
      field("px", r.price);
      field("qty", r.qty);
      field("removed", r.other_leaves);
      return s;
  }
  s += "UNKNOWN type=" + std::to_string(static_cast<unsigned>(r.type));
  return s;
}

}  // namespace me
