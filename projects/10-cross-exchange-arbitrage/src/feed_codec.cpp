#include "xarb/feed_codec.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

#include "xarb/crc32.hpp"
#include "xarb/json_lite.hpp"

namespace xarb {

namespace {

void put_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFFU));
  buf.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
  buf.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
  buf.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
}

void put_u64_le(std::vector<uint8_t>& buf, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    buf.push_back(static_cast<uint8_t>((v >> (i * 8U)) & 0xFFU));
  }
}

uint32_t get_u32_le(const uint8_t* p) noexcept {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

uint64_t get_u64_le(const uint8_t* p) noexcept {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (i * 8U);
  }
  return v;
}

}  // namespace

std::string encode_jsonl_event(const RecordedFeedEvent& ev) {
  std::string out;
  out.reserve(ev.payload.size() + 128);
  out += "{\"recv_ns\":";
  out += std::to_string(ev.recv_ns);
  out += ",\"venue\":\"";
  out += venue_name(ev.venue);
  out += "\",\"inst\":";
  out += std::to_string(ev.instrument_id);
  out += ",\"kind\":\"";
  out += feed_event_kind_name(ev.kind);
  out += "\",\"payload\":\"";
  out += json_escape(ev.payload);
  out += "\"}";
  return out;
}

std::optional<RecordedFeedEvent> decode_jsonl_event(std::string_view line, std::string* err) {
  auto parsed = parse_json(line);
  if (!parsed.ok() || !parsed.value->is_object()) {
    if (err) *err = parsed.ok() ? "jsonl line is not a JSON object" : parsed.error;
    return std::nullopt;
  }
  const auto& obj = *parsed.value;
  const auto* ts_v = obj.get("recv_ns");
  const auto* venue_v = obj.get("venue");
  const auto* inst_v = obj.get("inst");
  const auto* kind_v = obj.get("kind");
  const auto* payload_v = obj.get("payload");

  if (!ts_v || !ts_v->as_i64() || !venue_v || !venue_v->as_string() || !inst_v || !inst_v->as_u64() || !kind_v ||
      !kind_v->as_string() || !payload_v || !payload_v->as_string()) {
    if (err) *err = "missing required fields in jsonl event";
    return std::nullopt;
  }

  auto v = parse_venue_name(*venue_v->as_string());
  auto k = parse_feed_event_kind(*kind_v->as_string());
  if (!v || !k) {
    if (err) *err = "invalid venue or kind in jsonl event";
    return std::nullopt;
  }

  RecordedFeedEvent ev;
  ev.recv_ns = *ts_v->as_i64();
  ev.venue = *v;
  ev.instrument_id = static_cast<uint32_t>(*inst_v->as_u64());
  ev.kind = *k;
  ev.payload = *payload_v->as_string();
  return ev;
}

bool write_jsonl_feed(const std::string& path, const std::vector<RecordedFeedEvent>& events, std::string* err) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    if (err) *err = "failed to open jsonl feed for writing: " + path;
    return false;
  }
  for (const auto& ev : events) {
    ofs << encode_jsonl_event(ev) << '\n';
  }
  return ofs.good();
}

bool read_jsonl_feed(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err) {
  std::ifstream ifs(path);
  if (!ifs) {
    if (err) *err = "failed to open jsonl feed for reading: " + path;
    return false;
  }
  out.clear();
  std::string line;
  size_t line_no = 0;
  while (std::getline(ifs, line)) {
    ++line_no;
    if (line.empty()) continue;
    std::string parse_err;
    auto ev = decode_jsonl_event(line, &parse_err);
    if (!ev) {
      if (err) *err = "jsonl parse error on line " + std::to_string(line_no) + ": " + parse_err;
      return false;
    }
    out.push_back(std::move(*ev));
  }
  return true;
}

std::vector<uint8_t> encode_binary_frame(const RecordedFeedEvent& ev) {
  std::vector<uint8_t> buf;
  buf.reserve(kBinaryHeaderSize + ev.payload.size() + kBinaryFooterSize);
  put_u32_le(buf, kBinaryFrameMagic);
  buf.push_back(kBinaryFrameVersion);
  buf.push_back(static_cast<uint8_t>(ev.venue));
  buf.push_back(static_cast<uint8_t>(ev.kind));
  buf.push_back(0U);  // reserved
  put_u32_le(buf, ev.instrument_id);
  put_u64_le(buf, static_cast<uint64_t>(ev.recv_ns));
  put_u32_le(buf, static_cast<uint32_t>(ev.payload.size()));
  buf.insert(buf.end(), ev.payload.begin(), ev.payload.end());
  const uint32_t crc = Crc32::compute(buf.data(), buf.size());
  put_u32_le(buf, crc);
  return buf;
}

bool decode_binary_frame(const uint8_t* data, size_t len, size_t& offset, RecordedFeedEvent& out, std::string* err) {
  if (offset > len || len - offset < kBinaryHeaderSize + kBinaryFooterSize) {
    if (err) *err = "truncated binary frame header";
    return false;
  }
  const uint8_t* p = data + offset;
  const uint32_t magic = get_u32_le(p);
  if (magic != kBinaryFrameMagic) {
    if (err) *err = "invalid binary frame magic";
    return false;
  }
  const uint8_t version = p[4];
  if (version != kBinaryFrameVersion) {
    if (err) *err = "unsupported binary frame version";
    return false;
  }
  const uint8_t venue_raw = p[5];
  const uint8_t kind_raw = p[6];
  if (venue_raw >= static_cast<uint8_t>(VenueId::Count) || kind_raw > static_cast<uint8_t>(FeedEventKind::Reconnect)) {
    if (err) *err = "invalid venue or event kind in binary frame";
    return false;
  }
  const uint32_t inst_id = get_u32_le(p + 8);
  const int64_t recv_ns = static_cast<int64_t>(get_u64_le(p + 12));
  const uint32_t payload_len = get_u32_le(p + 20);
  if (payload_len > kMaxBinaryPayloadBytes) {
    if (err) *err = "binary frame payload exceeds max allowed size";
    return false;
  }
  const size_t total_len = kBinaryHeaderSize + static_cast<size_t>(payload_len) + kBinaryFooterSize;
  if (len - offset < total_len) {
    if (err) *err = "truncated binary frame payload";
    return false;
  }
  const uint32_t expected_crc = get_u32_le(p + kBinaryHeaderSize + payload_len);
  const uint32_t actual_crc = Crc32::compute(p, kBinaryHeaderSize + payload_len);
  if (expected_crc != actual_crc) {
    if (err) *err = "binary frame CRC32 mismatch";
    return false;
  }

  out.venue = static_cast<VenueId>(venue_raw);
  out.kind = static_cast<FeedEventKind>(kind_raw);
  out.instrument_id = inst_id;
  out.recv_ns = recv_ns;
  out.payload.assign(reinterpret_cast<const char*>(p + kBinaryHeaderSize), payload_len);
  offset += total_len;
  return true;
}

bool write_binary_feed(const std::string& path, const std::vector<RecordedFeedEvent>& events, std::string* err) {
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    if (err) *err = "failed to open binary feed for writing: " + path;
    return false;
  }
  for (const auto& ev : events) {
    auto frame = encode_binary_frame(ev);
    ofs.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
  }
  return ofs.good();
}

bool read_binary_feed(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err) {
  std::ifstream ifs(path, std::ios::binary | std::ios::ate);
  if (!ifs) {
    if (err) *err = "failed to open binary feed for reading: " + path;
    return false;
  }
  const std::streamsize size = ifs.tellg();
  if (size < 0) {
    if (err) *err = "failed to determine binary feed size: " + path;
    return false;
  }
  ifs.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (size > 0 && !ifs.read(reinterpret_cast<char*>(bytes.data()), size)) {
    if (err) *err = "failed to read binary feed bytes: " + path;
    return false;
  }
  out.clear();
  size_t offset = 0;
  while (offset < bytes.size()) {
    RecordedFeedEvent ev;
    if (!decode_binary_frame(bytes.data(), bytes.size(), offset, ev, err)) {
      return false;
    }
    out.push_back(std::move(ev));
  }
  return true;
}

bool read_feed_auto(const std::string& path, std::vector<RecordedFeedEvent>& out, std::string* err) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    if (err) *err = "failed to open feed: " + path;
    return false;
  }
  uint8_t magic[4] = {0, 0, 0, 0};
  ifs.read(reinterpret_cast<char*>(magic), 4);
  const auto n = ifs.gcount();
  ifs.close();
  if (n == 4 && get_u32_le(magic) == kBinaryFrameMagic) {
    return read_binary_feed(path, out, err);
  }
  return read_jsonl_feed(path, out, err);
}

}  // namespace xarb
