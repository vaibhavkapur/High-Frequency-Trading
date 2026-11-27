#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "obce/crc32.hpp"
#include "obce/delta_codec.hpp"
#include "obce/fixture_generator.hpp"
#include "obce/io_adapter.hpp"
#include "obce/reference_codec.hpp"
#include "obce/zstd_adapter.hpp"

namespace {

void print_usage() {
    std::cout
        << "Order Book Compression Engine CLI (obce_cli)\n"
        << "Usage:\n"
        << "  obce_cli demo [--events N] [--seed S] [--checkpoint K] [--schema 1|2]\n"
        << "  obce_cli encode <input.jsonl> <output.obce> [--codec ref|delta] [--schema 1|2] [--checkpoint K]\n"
        << "  obce_cli decode <input.obce> <output.jsonl> [--policy strict|allow|resync]\n"
        << "  obce_cli inspect <input.obce>\n"
        << "  obce_cli seek <input.obce> <seq_id>\n";
}

std::string hex32(uint32_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

int cmd_demo(int argc, char** argv) {
    obce::GeneratorConfig gen_cfg{};
    gen_cfg.seed = 20261127ULL;
    gen_cfg.event_count = 10000;
    gen_cfg.num_symbols = 8;
    gen_cfg.schema_version = obce::kSchemaVersionV2;
    gen_cfg.profile = obce::WorkloadProfile::L3OrderFlow;

    obce::CodecConfig codec_cfg{};
    codec_cfg.schema_version = obce::kSchemaVersionV2;
    codec_cfg.checkpoint_interval = 256;
    codec_cfg.delta_frame_events = 64;
    codec_cfg.stream_id = 1001;
    codec_cfg.gap_policy = obce::GapPolicy::StrictReject;
    codec_cfg.write_index_footer = true;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) {
            gen_cfg.event_count = std::stoull(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            gen_cfg.seed = std::stoull(argv[++i]);
        } else if (arg == "--checkpoint" && i + 1 < argc) {
            codec_cfg.checkpoint_interval = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--schema" && i + 1 < argc) {
            const uint16_t sv = static_cast<uint16_t>(std::stoul(argv[++i]));
            gen_cfg.schema_version = sv;
            codec_cfg.schema_version = sv;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            return 2;
        }
    }

    const auto cfg_check = obce::validate_config(codec_cfg);
    if (!cfg_check.ok()) {
        std::cerr << "Invalid config [" << obce::error_code_name(cfg_check.code) << "]: " << cfg_check.message << "\n";
        return 1;
    }

    const auto events = obce::generate_synthetic_events(gen_cfg);
    const uint32_t input_digest = obce::compute_events_digest(events, codec_cfg.schema_version);

    obce::DiagnosticCounters ref_enc_diag{};
    auto ref_enc = obce::ReferenceCodec::encode_stream(events, codec_cfg, &ref_enc_diag);
    if (!ref_enc.ok()) {
        std::cerr << "Reference encode failed: " << ref_enc.status.message << "\n";
        return 1;
    }
    auto ref_dec = obce::ReferenceCodec::decode_stream(*ref_enc.value);
    if (!ref_dec.ok() || *ref_dec.value != events) {
        std::cerr << "Reference roundtrip verification failed!\n";
        return 1;
    }

    auto delta_enc = obce::DeltaEncoder::encode_all(events, codec_cfg);
    if (!delta_enc.ok()) {
        std::cerr << "Delta encode failed: " << delta_enc.status.message << "\n";
        return 1;
    }
    auto delta_dec = obce::DeltaDecoder::decode_all(delta_enc.value->bytes, obce::GapPolicy::StrictReject);
    if (!delta_dec.ok() || delta_dec.value->events != events) {
        std::cerr << "Delta roundtrip verification failed!\n";
        return 1;
    }

    const uint32_t output_digest = obce::compute_events_digest(delta_dec.value->events, codec_cfg.schema_version);
    const double ref_bpe = static_cast<double>(ref_enc.value->size()) / static_cast<double>(events.size());
    const double delta_bpe = static_cast<double>(delta_enc.value->bytes.size()) / static_cast<double>(events.size());
    const double ratio = static_cast<double>(ref_enc.value->size()) / static_cast<double>(delta_enc.value->bytes.size());

    // Demonstrate random-access seek to middle sequence ID
    obce::StreamHeader idx_hdr{};
    auto idx_res = obce::DeltaDecoder::read_checkpoint_index(delta_enc.value->bytes.data(),
                                                             delta_enc.value->bytes.size(),
                                                             &idx_hdr);
    if (!idx_res.ok()) {
        std::cerr << "Checkpoint index read failed: " << idx_res.status.message << "\n";
        return 1;
    }
    const uint64_t mid_seq = events[events.size() / 2].seq_id;
    obce::DiagnosticCounters seek_diag{};
    auto seek_res = obce::DeltaDecoder::seek_by_seq_id(
        delta_enc.value->bytes.data(), delta_enc.value->bytes.size(),
        mid_seq, *idx_res.value, idx_hdr, &seek_diag);
    if (!seek_res.ok() || *seek_res.value != events[events.size() / 2]) {
        std::cerr << "Random-access seek verification failed!\n";
        return 1;
    }

    // Demonstrate recovery from missing/corrupt delta frame
    std::vector<uint8_t> damaged = delta_enc.value->bytes;
    if (delta_enc.value->checkpoints.size() >= 2) {
        // Corrupt a byte inside the first delta frame after checkpoint #0
        const size_t first_delta_off = static_cast<size_t>(delta_enc.value->checkpoints[0].byte_offset +
                                                           delta_enc.value->checkpoints[0].frame_length);
        if (first_delta_off + 24 < damaged.size()) {
            damaged[first_delta_off + 22] ^= 0xFFU;
        }
    }
    auto strict_fail = obce::DeltaDecoder::decode_all(damaged, obce::GapPolicy::StrictReject);
    auto resync_ok = obce::DeltaDecoder::decode_all(damaged, obce::GapPolicy::ResyncAtCheckpoint);

    std::cout << "=== Order Book Compression Engine Demo ===\n"
              << "Workload:              " << obce::workload_profile_name(gen_cfg.profile)
              << " (synthetic, seed=" << gen_cfg.seed << ", events=" << events.size() << ")\n"
              << "Schema version:        v" << codec_cfg.schema_version
              << " (price_decimals=" << static_cast<int>(codec_cfg.price_decimals) << ")\n"
              << "Checkpoint interval:   " << codec_cfg.checkpoint_interval
              << " (" << idx_res.value->size() << " checkpoints)\n"
              << "Optional zstd adapter: " << obce::ZstdAdapter::version_string() << "\n"
              << "------------------------------------------\n"
              << std::fixed << std::setprecision(2)
              << "Reference binary:      " << ref_enc.value->size() << " bytes (" << ref_bpe << " bytes/event)\n"
              << "Delta+Varint stream:   " << delta_enc.value->bytes.size() << " bytes (" << delta_bpe << " bytes/event)\n"
              << "Compression ratio:     " << ratio << "x vs reference binary\n"
              << "Input digest (CRC32C): " << hex32(input_digest) << "\n"
              << "Decoded digest:        " << hex32(output_digest) << " (MATCH=" << (input_digest == output_digest ? "YES" : "NO") << ")\n"
              << "Random-access seek:    seq_id=" << mid_seq
              << " found after scanning " << seek_diag.events_processed << " events ("
              << seek_diag.frames_processed << " frames)\n"
              << "Corrupt-frame strict:  rejected with " << obce::error_code_name(strict_fail.status.code) << "\n"
              << "Corrupt-frame resync:  recovered " << (resync_ok.ok() ? resync_ok.value->events.size() : 0)
              << "/" << events.size() << " events (resync_recoveries="
              << (resync_ok.ok() ? resync_ok.value->counters.resync_recoveries : 0) << ")\n"
              << "Encoder diagnostics:   " << obce::format_diagnostics_json(delta_enc.value->counters) << "\n";
    return 0;
}

int cmd_encode(int argc, char** argv) {
    if (argc < 4) {
        print_usage();
        return 2;
    }
    const std::string in_path = argv[2];
    const std::string out_path = argv[3];
    std::string codec = "delta";
    obce::CodecConfig cfg{};
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--codec" && i + 1 < argc) codec = argv[++i];
        else if (arg == "--schema" && i + 1 < argc) cfg.schema_version = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if (arg == "--checkpoint" && i + 1 < argc) cfg.checkpoint_interval = static_cast<uint32_t>(std::stoul(argv[++i]));
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    auto ev_res = obce::load_events_jsonl(in_path);
    if (!ev_res.ok()) {
        std::cerr << "Failed to load input [" << obce::error_code_name(ev_res.status.code) << "]: "
                  << ev_res.status.message << "\n";
        return 1;
    }

    if (codec == "ref") {
        obce::DiagnosticCounters diag{};
        auto enc = obce::ReferenceCodec::encode_stream(*ev_res.value, cfg, &diag);
        if (!enc.ok()) {
            std::cerr << "Encode failed [" << obce::error_code_name(enc.status.code) << "]: " << enc.status.message << "\n";
            return 1;
        }
        auto w = obce::write_binary_file(out_path, *enc.value);
        if (!w.ok()) {
            std::cerr << "Write failed: " << w.message << "\n";
            return 1;
        }
        std::cout << "Encoded " << ev_res.value->size() << " events (reference) -> " << enc.value->size()
                  << " bytes; diagnostics=" << obce::format_diagnostics_json(diag) << "\n";
        return 0;
    } else if (codec == "delta") {
        auto enc = obce::DeltaEncoder::encode_all(*ev_res.value, cfg);
        if (!enc.ok()) {
            std::cerr << "Encode failed [" << obce::error_code_name(enc.status.code) << "]: " << enc.status.message << "\n";
            return 1;
        }
        auto w = obce::write_binary_file(out_path, enc.value->bytes);
        if (!w.ok()) {
            std::cerr << "Write failed: " << w.message << "\n";
            return 1;
        }
        std::cout << "Encoded " << ev_res.value->size() << " events (delta) -> " << enc.value->bytes.size()
                  << " bytes; diagnostics=" << obce::format_diagnostics_json(enc.value->counters) << "\n";
        return 0;
    }
    std::cerr << "Unsupported codec option: " << codec << "\n";
    return 2;
}

int cmd_decode(int argc, char** argv) {
    if (argc < 4) {
        print_usage();
        return 2;
    }
    const std::string in_path = argv[2];
    const std::string out_path = argv[3];
    obce::GapPolicy policy = obce::GapPolicy::StrictReject;
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--policy" && i + 1 < argc) {
            std::string p = argv[++i];
            if (p == "strict") policy = obce::GapPolicy::StrictReject;
            else if (p == "allow") policy = obce::GapPolicy::AllowAndFlag;
            else if (p == "resync") policy = obce::GapPolicy::ResyncAtCheckpoint;
            else {
                std::cerr << "Invalid policy: " << p << "\n";
                return 2;
            }
        }
    }

    auto bin = obce::read_binary_file(in_path);
    if (!bin.ok()) {
        std::cerr << "Read failed: " << bin.status.message << "\n";
        return 1;
    }
    size_t off = 0;
    auto hdr = obce::decode_stream_header(bin.value->data(), bin.value->size(), off);
    if (!hdr.ok()) {
        std::cerr << "Header decode failed [" << obce::error_code_name(hdr.status.code) << "]: "
                  << hdr.status.message << "\n";
        return 1;
    }

    if (hdr.value->codec == obce::CodecKind::ReferenceBinary) {
        obce::DiagnosticCounters diag{};
        auto dec = obce::ReferenceCodec::decode_stream(*bin.value, policy, &diag);
        if (!dec.ok()) {
            std::cerr << "Decode failed [" << obce::error_code_name(dec.status.code) << "]: "
                      << dec.status.message << "\n";
            return 1;
        }
        auto w = obce::save_events_jsonl(out_path, *dec.value, hdr.value->schema_version);
        if (!w.ok()) {
            std::cerr << "Write failed: " << w.message << "\n";
            return 1;
        }
        std::cout << "Decoded " << dec.value->size() << " events; diagnostics=" << obce::format_diagnostics_json(diag) << "\n";
        return 0;
    } else {
        auto dec = obce::DeltaDecoder::decode_all(*bin.value, policy);
        if (!dec.ok()) {
            std::cerr << "Decode failed [" << obce::error_code_name(dec.status.code) << "]: "
                      << dec.status.message << "\n";
            return 1;
        }
        auto w = obce::save_events_jsonl(out_path, dec.value->events, obce::kSchemaVersionV2);
        if (!w.ok()) {
            std::cerr << "Write failed: " << w.message << "\n";
            return 1;
        }
        std::cout << "Decoded " << dec.value->events.size() << " events; diagnostics="
                  << obce::format_diagnostics_json(dec.value->counters) << "\n";
        return 0;
    }
}

int cmd_inspect(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 2;
    }
    auto bin = obce::read_binary_file(argv[2]);
    if (!bin.ok()) {
        std::cerr << "Read failed: " << bin.status.message << "\n";
        return 1;
    }
    size_t off = 0;
    auto hdr = obce::decode_stream_header(bin.value->data(), bin.value->size(), off);
    if (!hdr.ok()) {
        std::cerr << "Invalid stream [" << obce::error_code_name(hdr.status.code) << "]: "
                  << hdr.status.message << "\n";
        return 1;
    }
    std::cout << "File bytes:          " << bin.value->size() << "\n"
              << "Schema version:      v" << hdr.value->schema_version << "\n"
              << "Codec:               " << (hdr.value->codec == obce::CodecKind::ReferenceBinary ? "ReferenceBinary" : "DeltaVarint") << "\n"
              << "Price decimals:      " << static_cast<int>(hdr.value->price_decimals) << "\n"
              << "Checkpoint interval: " << hdr.value->checkpoint_interval << "\n"
              << "Stream ID:           " << hdr.value->stream_id << "\n";
    if (hdr.value->codec == obce::CodecKind::DeltaVarint) {
        auto idx = obce::DeltaDecoder::read_checkpoint_index(bin.value->data(), bin.value->size());
        if (idx.ok()) {
            std::cout << "Checkpoints:         " << idx.value->size() << "\n";
        }
    }
    return 0;
}

int cmd_seek(int argc, char** argv) {
    if (argc < 4) {
        print_usage();
        return 2;
    }
    auto bin = obce::read_binary_file(argv[2]);
    if (!bin.ok()) {
        std::cerr << "Read failed: " << bin.status.message << "\n";
        return 1;
    }
    const uint64_t target_seq = std::stoull(argv[3]);
    obce::StreamHeader hdr{};
    auto idx = obce::DeltaDecoder::read_checkpoint_index(bin.value->data(), bin.value->size(), &hdr);
    if (!idx.ok()) {
        std::cerr << "Index read failed: " << idx.status.message << "\n";
        return 1;
    }
    obce::DiagnosticCounters diag{};
    auto ev = obce::DeltaDecoder::seek_by_seq_id(bin.value->data(), bin.value->size(), target_seq, *idx.value, hdr, &diag);
    if (!ev.ok()) {
        std::cerr << "Seek failed [" << obce::error_code_name(ev.status.code) << "]: " << ev.status.message << "\n";
        return 1;
    }
    std::cout << obce::event_to_json_line(*ev.value, hdr.schema_version) << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "demo") return cmd_demo(argc, argv);
    if (cmd == "encode") return cmd_encode(argc, argv);
    if (cmd == "decode") return cmd_decode(argc, argv);
    if (cmd == "inspect") return cmd_inspect(argc, argv);
    if (cmd == "seek") return cmd_seek(argc, argv);
    print_usage();
    return 2;
}
