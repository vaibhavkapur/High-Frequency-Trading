#include "mmsim/feed.hpp"

#include "mmsim/protocol.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <sstream>

namespace mmsim {

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool load_bin_file(const std::string& path, std::vector<MarketEvent>& out,
                   double* out_tick_size_dollars, std::string& err) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        err = "Failed to open binary feed: " + path;
        return false;
    }
    ifs.seekg(0, std::ios::end);
    const auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    if (file_size < static_cast<std::streamoff>(kBinHeaderSize)) {
        err = "Binary feed truncated before header: " + path;
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    ifs.read(reinterpret_cast<char*>(buf.data()), file_size);
    if (!ifs) {
        err = "Failed reading binary feed: " + path;
        return false;
    }

    BinFileHeader hdr{};
    const DecodeStatus hstatus = decode_bin_header(buf, hdr);
    if (hstatus != DecodeStatus::Ok) {
        err = "Invalid binary header (" + decode_status_to_string(hstatus) + "): " + path;
        return false;
    }
    if (out_tick_size_dollars != nullptr) {
        *out_tick_size_dollars = static_cast<double>(hdr.tick_size_micros) / 1'000'000.0;
    }

    const size_t payload_bytes = buf.size() - kBinHeaderSize;
    if (payload_bytes % kBinRecordSize != 0) {
        err = "Binary feed has trailing partial record bytes: " + path;
        return false;
    }
    const size_t n_records = payload_bytes / kBinRecordSize;
    out.clear();
    out.reserve(n_records);

    TimestampNs prev_ts = 0;
    for (size_t i = 0; i < n_records; ++i) {
        MarketEvent ev{};
        const auto slice = std::span<const uint8_t>(buf.data() + kBinHeaderSize + i * kBinRecordSize,
                                                    kBinRecordSize);
        const DecodeStatus rstatus = decode_bin_record(slice, ev);
        if (rstatus != DecodeStatus::Ok) {
            err = "Corrupt record #" + std::to_string(i) + " (" + decode_status_to_string(rstatus) +
                  ") in " + path;
            return false;
        }
        if (i > 0 && ev.timestamp_ns < prev_ts) {
            err = "Out-of-order timestamp at record #" + std::to_string(i) + " in " + path;
            return false;
        }
        prev_ts = ev.timestamp_ns;
        out.push_back(ev);
    }
    return true;
}

bool load_csv_file(const std::string& path, std::vector<MarketEvent>& out, std::string& err) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        err = "Failed to open CSV feed: " + path;
        return false;
    }
    out.clear();
    std::string line;
    int line_no = 0;
    TimestampNs prev_ts = 0;
    bool have_prev = false;

    while (std::getline(ifs, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line_no == 1 && line.rfind("timestamp_ns", 0) == 0) {
            continue; // Header row
        }
        std::stringstream ss(line);
        std::string f_ts, f_seq, f_type, f_side, f_price, f_qty;
        if (!std::getline(ss, f_ts, ',') || !std::getline(ss, f_seq, ',') ||
            !std::getline(ss, f_type, ',') || !std::getline(ss, f_side, ',') ||
            !std::getline(ss, f_price, ',') || !std::getline(ss, f_qty, ',')) {
            err = "Malformed CSV line " + std::to_string(line_no) + " in " + path;
            return false;
        }

        MarketEvent ev{};
        try {
            ev.timestamp_ns = std::stoll(f_ts);
            ev.seq_no = std::stoull(f_seq);
            ev.price_ticks = std::stoll(f_price);
            ev.qty = std::stoll(f_qty);
        } catch (const std::exception& e) {
            err = "Numeric parse error on line " + std::to_string(line_no) + ": " + e.what();
            return false;
        }

        if (f_type == "BOOK" || f_type == "BOOK_DELTA" || f_type == "DELTA") {
            ev.type = MarketEventType::BookDelta;
        } else if (f_type == "TRADE") {
            ev.type = MarketEventType::Trade;
        } else if (f_type == "CLEAR" || f_type == "BOOK_CLEAR") {
            ev.type = MarketEventType::BookClear;
        } else {
            err = "Unknown event type '" + f_type + "' on line " + std::to_string(line_no);
            return false;
        }

        if (f_side == "BID" || f_side == "B" || f_side == "0") {
            ev.side = Side::Bid;
        } else if (f_side == "ASK" || f_side == "A" || f_side == "1") {
            ev.side = Side::Ask;
        } else {
            err = "Unknown side '" + f_side + "' on line " + std::to_string(line_no);
            return false;
        }

        if (ev.timestamp_ns < 0 || ev.qty < 0) {
            err = "Negative timestamp/qty on line " + std::to_string(line_no);
            return false;
        }
        if (have_prev && ev.timestamp_ns < prev_ts) {
            err = "Out-of-order timestamp on line " + std::to_string(line_no);
            return false;
        }
        prev_ts = ev.timestamp_ns;
        have_prev = true;
        out.push_back(ev);
    }
    return true;
}

} // namespace

bool load_market_events(const std::string& path, std::vector<MarketEvent>& out,
                        double* out_tick_size_dollars, std::string& err) {
    if (ends_with(path, ".mmbin") || ends_with(path, ".bin")) {
        return load_bin_file(path, out, out_tick_size_dollars, err);
    }
    return load_csv_file(path, out, err);
}

bool save_market_events_bin(const std::string& path, const std::vector<MarketEvent>& events,
                            double tick_size_dollars, std::string& err) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to open output binary file: " + path;
        return false;
    }
    BinFileHeader hdr{};
    hdr.version = kBinVersion;
    hdr.flags = 0;
    hdr.tick_size_micros = static_cast<uint32_t>(std::llround(tick_size_dollars * 1'000'000.0));
    if (hdr.tick_size_micros == 0) {
        hdr.tick_size_micros = 10'000;
    }
    std::array<uint8_t, kBinHeaderSize> hbuf{};
    encode_bin_header(hdr, hbuf);
    ofs.write(reinterpret_cast<const char*>(hbuf.data()), static_cast<std::streamsize>(hbuf.size()));

    std::array<uint8_t, kBinRecordSize> rbuf{};
    for (const auto& ev : events) {
        encode_bin_record(ev, rbuf);
        ofs.write(reinterpret_cast<const char*>(rbuf.data()),
                  static_cast<std::streamsize>(rbuf.size()));
    }
    if (!ofs) {
        err = "Failed writing binary feed: " + path;
        return false;
    }
    return true;
}

bool save_market_events_csv(const std::string& path, const std::vector<MarketEvent>& events,
                            std::string& err) {
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        err = "Failed to open output CSV file: " + path;
        return false;
    }
    ofs << "timestamp_ns,seq_no,type,side,price_ticks,qty\n";
    for (const auto& ev : events) {
        const char* type_str = "BOOK";
        if (ev.type == MarketEventType::Trade) {
            type_str = "TRADE";
        } else if (ev.type == MarketEventType::BookClear) {
            type_str = "CLEAR";
        }
        ofs << ev.timestamp_ns << ',' << ev.seq_no << ',' << type_str << ','
            << side_to_string(ev.side) << ',' << ev.price_ticks << ',' << ev.qty << '\n';
    }
    if (!ofs) {
        err = "Failed writing CSV feed: " + path;
        return false;
    }
    return true;
}

} // namespace mmsim
