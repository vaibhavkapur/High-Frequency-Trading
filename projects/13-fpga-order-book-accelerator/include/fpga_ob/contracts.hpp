#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fpga_ob {

inline constexpr uint8_t  kSchemaVersion          = 1;
inline constexpr uint32_t kDefaultPriceBase       = 10000;
inline constexpr uint32_t kDefaultNumLevels       = 64;
inline constexpr uint32_t kDefaultMaxActiveLevels = 32;
inline constexpr size_t   kWireEventSize          = 20;

enum class Opcode : uint8_t {
    Nop     = 0x00,
    Add     = 0x01,
    Cancel  = 0x02,
    Execute = 0x03,
};

enum class Side : uint8_t {
    Bid = 0x00,
    Ask = 0x01,
};

enum class Status : uint8_t {
    Ok                   = 0x00,
    ErrUnsupportedSchema = 0x01,
    ErrUnsupportedFlags  = 0x02,
    ErrUnsupportedOpcode = 0x03,
    ErrInvalidSide       = 0x04,
    ErrPriceOob          = 0x05,
    ErrZeroQty           = 0x06,
    ErrFullCapacity      = 0x07,
    ErrOverflowQty       = 0x08,
    ErrOverflowOrders    = 0x09,
    ErrInvalidCancel     = 0x0A,
    ErrInvalidExecute    = 0x0B,
};

struct Config {
    uint32_t price_base        = kDefaultPriceBase;
    uint32_t num_levels        = kDefaultNumLevels;
    uint32_t max_active_levels = kDefaultMaxActiveLevels;

    [[nodiscard]] std::optional<std::string> validate() const;
};

struct EventPacket {
    uint32_t seq_no       = 0;
    uint32_t timestamp_ns = 0;
    uint32_t price        = 0;
    uint32_t qty          = 0;
    uint8_t  opcode       = static_cast<uint8_t>(Opcode::Nop);
    uint8_t  side         = static_cast<uint8_t>(Side::Bid);
    uint8_t  flags        = 0;
    uint8_t  schema_ver   = kSchemaVersion;

    bool operator==(const EventPacket& o) const = default;
};

struct BboOutputPacket {
    uint8_t  schema_ver        = kSchemaVersion;
    bool     lane1_valid       = false;
    Status   status_0          = Status::Ok;
    Status   status_1          = Status::Ok;
    uint32_t seq_no_0          = 0;
    uint32_t seq_no_1          = 0;
    uint32_t timestamp_ns      = 0;
    bool     best_bid_valid    = false;
    uint32_t best_bid_price    = 0;
    uint32_t best_bid_qty      = 0;
    uint16_t best_bid_orders   = 0;
    bool     best_ask_valid    = false;
    uint32_t best_ask_price    = 0;
    uint32_t best_ask_qty      = 0;
    uint16_t best_ask_orders   = 0;
    int32_t  spread            = 0;
    bool     crossed           = false;
    uint16_t active_bid_levels = 0;
    uint16_t active_ask_levels = 0;

    bool operator==(const BboOutputPacket& o) const = default;
};

// Wire binary serialization (20-byte fixed-width little-endian frame)
std::array<uint8_t, kWireEventSize> encode_event(const EventPacket& pkt);
EventPacket decode_event(const std::array<uint8_t, kWireEventSize>& bytes);

// Enum string helpers
const char* status_name(Status st);
const char* opcode_name(uint8_t op);
const char* side_name(uint8_t side);
Status parse_status_name(const std::string& name);
uint8_t parse_opcode_name(const std::string& name);
uint8_t parse_side_name(const std::string& name);

// JSON & binary fixture I/O adapters (isolated from hot-path state machine)
std::string to_json(const BboOutputPacket& out);
std::string to_json(const EventPacket& ev);

struct FixtureBundle {
    Config                   config;
    std::string              name;
    std::string              schema_version;
    std::vector<EventPacket> events;
};

FixtureBundle load_events_json(const std::string& path);
std::vector<BboOutputPacket> load_expected_json(const std::string& path);
void write_binary_stream(const std::string& path, const std::vector<EventPacket>& events);
std::vector<EventPacket> read_binary_stream(const std::string& path);

}  // namespace fpga_ob
