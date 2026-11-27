#include "kbnl/benchmark_runner.hpp"
#include "kbnl/pacer.hpp"
#include "kbnl/posix_udp_transport.hpp"
#include "kbnl/ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

namespace kbnl {

namespace {

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

std::string make_run_id(const BenchmarkConfig& cfg, const std::string& config_hash) {
    return cfg.workload.scenario_name + "_" + std::string(backend_name(cfg.workload.backend)) +
           "_" + config_hash.substr(0, 8);
}

} // namespace

BenchmarkResult run_benchmark(const BenchmarkConfig& config) {
    BenchmarkResult res{};
    res.config = config;
    res.config_hash = compute_config_hash(config);
    res.run_id = make_run_id(config, res.config_hash);
    res.timestamp_utc = utc_timestamp_iso8601();
    res.scenario_name = config.workload.scenario_name;
    res.backend = std::string(backend_name(config.workload.backend));
    res.is_simulated = false;
    res.clock = get_clock_metadata();
    res.expected_warmup_sent = config.workload.warmup_packets;
    res.expected_measured_sent = config.workload.measured_packets;

    const auto validation_errors = validate_config(config);
    if (!validation_errors.empty()) {
        res.execution_status = "FAILED";
        res.status = Status::Error(
            StatusCode::kInvalidConfig,
            validation_errors.front().field + ": " + validation_errors.front().message);
        return res;
    }

    res.capability = probe_backend_capability(config.workload.backend, config.topology);
    if (!res.capability.runtime_available) {
        res.execution_status = "NOT_RUN";
        res.status = Status::Error(res.capability.reason_code, res.capability.reason);
        return res;
    }

    Status create_status = Status::Ok();
    auto receiver = create_receiver(config.workload.backend, create_status);
    if (!create_status.ok() || !receiver) {
        res.execution_status = "NOT_RUN";
        res.status = create_status;
        return res;
    }

    const Status open_rx = receiver->open(config.topology);
    if (!open_rx.ok()) {
        res.execution_status = (open_rx.code == StatusCode::kUnsupportedPlatform ||
                                open_rx.code == StatusCode::kFeatureDisabled ||
                                open_rx.code == StatusCode::kMissingCapability ||
                                open_rx.code == StatusCode::kDeviceUnavailable)
                                   ? "NOT_RUN"
                                   : "FAILED";
        res.status = open_rx;
        return res;
    }

    const uint16_t active_port = receiver->bound_port();
    PosixUdpSender sender;
    const Status open_tx = sender.open(config.topology, active_port);
    if (!open_tx.ok()) {
        receiver->close();
        res.execution_status = "FAILED";
        res.status = open_tx;
        return res;
    }

    SpscRingBuffer<ReceivedFrame> ring(config.topology.ring_capacity);
    SequenceTracker tracker;
    LatencyStats latency_stats;

    std::atomic<bool> rx_ready{false};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> rx_io_done{false};
    std::atomic<bool> abort_flag{false};
    Status rx_error = Status::Ok();
    Status tx_error = Status::Ok();
    PinResult receiver_pin_result{};
    PinResult sender_pin_result{};

    uint64_t actual_sent = 0;
    uint64_t send_errors = 0;
    uint64_t measured_start_ns = 0;
    uint64_t measured_end_ns = 0;

    // Receiver I/O thread: polls backend and enqueues frames into lock-free SPSC ring.
    std::thread rx_io_thread([&]() {
        if (config.topology.pin_threads && config.topology.receiver_cpu >= 0) {
            receiver_pin_result = pin_current_thread(config.topology.receiver_cpu);
            if (config.topology.strict_pinning && !receiver_pin_result.applied) {
                rx_error = Status::Error(StatusCode::kUnsupportedPlatform,
                                         "Strict receiver pinning failed: " + receiver_pin_result.detail);
                abort_flag.store(true, std::memory_order_release);
                rx_ready.store(true, std::memory_order_release);
                rx_io_done.store(true, std::memory_order_release);
                return;
            }
        } else {
            receiver_pin_result = pin_current_thread(-1);
        }

        rx_ready.store(true, std::memory_order_release);

        std::vector<ReceivedFrame> batch;
        batch.reserve(config.workload.batch_size);
        bool eos_seen = false;
        uint64_t post_sender_deadline = 0;

        while (!abort_flag.load(std::memory_order_acquire)) {
            Status poll_st = Status::Ok();
            const uint32_t wait_ms = sender_done.load(std::memory_order_acquire) ? 5u : 10u;
            const size_t n = receiver->poll_batch(batch, config.workload.batch_size, wait_ms, poll_st);
            if (!poll_st.ok()) {
                rx_error = poll_st;
                abort_flag.store(true, std::memory_order_release);
                break;
            }
            for (size_t i = 0; i < n; ++i) {
                // Quick peek for EOS flag so receiver can stop cleanly once drained.
                if (batch[i].length >= kMinPacketBytes) {
                    const uint16_t flags = static_cast<uint16_t>(
                        (static_cast<uint16_t>(batch[i].data[6]) << 8) | batch[i].data[7]);
                    if ((flags & kFlagEndOfStream) != 0) {
                        eos_seen = true;
                    }
                }
                (void)ring.try_push(std::move(batch[i]));
            }
            if (sender_done.load(std::memory_order_acquire)) {
                if (post_sender_deadline == 0) {
                    post_sender_deadline =
                        now_ns() + static_cast<uint64_t>(config.workload.drain_timeout_ms) * 1000000ULL;
                }
                if (eos_seen && n == 0) {
                    break;
                }
                if (now_ns() >= post_sender_deadline) {
                    break;
                }
            }
        }
        rx_io_done.store(true, std::memory_order_release);
    });

    // Pure domain consumer thread: pops frames from SPSC ring, validates, tracks sequence & latency.
    std::thread consumer_thread([&]() {
        ReceivedFrame frame{};
        while (true) {
            if (ring.try_pop(frame)) {
                DecodedPacket pkt{};
                const DecodeError err = decode_packet(
                    std::span<const uint8_t>(frame.data.data(), frame.length), pkt);
                if (err != DecodeError::kOk) {
                    tracker.on_decode_error(err);
                    continue;
                }
                const PacketObservation obs = tracker.on_packet(pkt.header);
                if (obs == PacketObservation::kInOrder ||
                    obs == PacketObservation::kGapDetected ||
                    obs == PacketObservation::kOutOfOrder) {
                    const bool is_warmup = (pkt.header.flags & kFlagWarmup) != 0;
                    latency_stats.record_sample(pkt.header.send_ts_ns, frame.recv_ts_ns, is_warmup);
                }
            } else if (rx_io_done.load(std::memory_order_acquire) && ring.empty_approx()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Wait for receiver thread to be ready before sending.
    while (!rx_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // Sender thread: paced deterministic UDP feed.
    std::thread tx_thread([&]() {
        if (abort_flag.load(std::memory_order_acquire)) {
            sender_done.store(true, std::memory_order_release);
            return;
        }
        if (config.topology.pin_threads && config.topology.sender_cpu >= 0) {
            sender_pin_result = pin_current_thread(config.topology.sender_cpu);
            if (config.topology.strict_pinning && !sender_pin_result.applied) {
                tx_error = Status::Error(StatusCode::kUnsupportedPlatform,
                                         "Strict sender pinning failed: " + sender_pin_result.detail);
                abort_flag.store(true, std::memory_order_release);
                sender_done.store(true, std::memory_order_release);
                return;
            }
        } else {
            sender_pin_result = pin_current_thread(-1);
        }

        const PacerSchedule schedule(config.workload);
        const uint64_t total_pkts = schedule.total_data_packets();
        std::vector<uint8_t> buf(config.workload.packet_bytes);

        const uint64_t start_ns = now_ns();
        for (uint64_t i = 0; i < total_pkts && !abort_flag.load(std::memory_order_acquire); ++i) {
            const uint64_t target_ns = start_ns + schedule.scheduled_offset_ns(i);
            pace_until_ns(target_ns);

            const bool is_warmup = schedule.is_warmup(i);
            uint16_t flags = is_warmup ? kFlagWarmup : kFlagNone;
            if (schedule.is_burst_packet(i)) {
                flags = static_cast<uint16_t>(flags | kFlagBurst);
            }

            const uint64_t send_ts = now_ns();
            if (!is_warmup && measured_start_ns == 0) {
                measured_start_ns = send_ts;
            }

            encode_deterministic_packet(
                i + 1,
                send_ts,
                config.workload.feed_id,
                flags,
                config.workload.seed,
                config.workload.packet_bytes,
                buf);

            const Status st = sender.send_packet(buf);
            if (st.ok()) {
                ++actual_sent;
                if (!is_warmup) {
                    measured_end_ns = send_ts;
                }
            } else {
                ++send_errors;
                if (tx_error.ok()) {
                    tx_error = st;
                }
            }
        }

        // Send 3 End-Of-Stream control markers spaced 1ms apart for clean receiver drain.
        for (int e = 0; e < 3 && !abort_flag.load(std::memory_order_acquire); ++e) {
            encode_deterministic_packet(
                0,
                now_ns(),
                config.workload.feed_id,
                kFlagEndOfStream,
                config.workload.seed,
                config.workload.packet_bytes,
                buf);
            (void)sender.send_packet(buf);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        sender_done.store(true, std::memory_order_release);
    });

    tx_thread.join();
    rx_io_thread.join();
    consumer_thread.join();

    sender.close();
    receiver->close();

    tracker.on_ring_drop(ring.dropped_count());

    res.sender_pin = sender_pin_result;
    res.receiver_pin = receiver_pin_result;

    if (!rx_error.ok()) {
        res.execution_status = "FAILED";
        res.status = rx_error;
        return res;
    }
    if (!tx_error.ok() && actual_sent == 0) {
        res.execution_status = "FAILED";
        res.status = tx_error;
        res.send_errors = send_errors;
        return res;
    }

    res.execution_status = "MEASURED";
    res.status = Status::Ok();
    res.actual_packets_sent = actual_sent;
    res.send_errors = send_errors;
    res.measured_duration_ns =
        (measured_end_ns > measured_start_ns) ? (measured_end_ns - measured_start_ns) : 1ULL;

    const double dur_sec = static_cast<double>(res.measured_duration_ns) / 1e9;
    res.sequence = tracker.counters();
    res.inferred_total_lost = res.sequence.lost_against_expected(actual_sent);
    res.loss_ratio = (actual_sent > 0)
                         ? static_cast<double>(res.inferred_total_lost) / static_cast<double>(actual_sent)
                         : 0.0;
    res.achieved_send_pps = (dur_sec > 0.0) ? static_cast<double>(config.workload.measured_packets) / dur_sec : 0.0;
    res.achieved_recv_pps = (dur_sec > 0.0) ? static_cast<double>(res.sequence.measured_received) / dur_sec : 0.0;
    res.achieved_recv_mbps =
        (res.achieved_recv_pps * static_cast<double>(config.workload.packet_bytes) * 8.0) / 1e6;

    // Verify sequence accounting:
    // 1. Internal tracker invariant holds.
    // 2. unique_received + inferred_total_lost == actual_sent.
    res.sequence_accounting_verified =
        res.sequence.accounting_invariant_holds() &&
        (res.sequence.unique_received + res.inferred_total_lost == actual_sent);

    res.latency = latency_stats.summarize();
    return res;
}

BenchmarkResult run_fixture_replay(const std::string& fixture_path, size_t ring_capacity) {
    BenchmarkResult res{};
    res.scenario_name = "deterministic_fixture_replay";
    res.backend = "fixture_replay";
    res.run_id = "fixture_replay_v1";
    res.timestamp_utc = utc_timestamp_iso8601();
    res.is_simulated = false;
    res.clock = get_clock_metadata();
    res.config.fixture_path = fixture_path;
    res.config.topology.ring_capacity = ring_capacity;
    res.config_hash = compute_config_hash(res.config);

    std::ifstream ifs(fixture_path, std::ios::binary);
    if (!ifs.is_open()) {
        res.execution_status = "FAILED";
        res.status = Status::Error(StatusCode::kInvalidConfig, "Unable to open fixture: " + fixture_path);
        return res;
    }

    // Binary fixture format:
    //   Header: 8 bytes "KBNLFIX1", uint32_t record_count (LE), uint32_t expected_total_sent (LE)
    //   Records (repeated record_count times):
    //     uint64_t recv_ts_ns (LE)
    //     uint32_t frame_len (LE)
    //     uint8_t  frame_bytes[frame_len]
    uint8_t file_hdr[16];
    ifs.read(reinterpret_cast<char*>(file_hdr), sizeof(file_hdr));
    if (!ifs.good() || std::memcmp(file_hdr, "KBNLFIX1", 8) != 0) {
        res.execution_status = "FAILED";
        res.status = Status::Error(StatusCode::kInvalidConfig, "Invalid fixture file header");
        return res;
    }

    const uint32_t record_count = read_u32_le(file_hdr + 8);
    const uint32_t expected_total_sent = read_u32_le(file_hdr + 12);

    SpscRingBuffer<ReceivedFrame> ring(ring_capacity);
    SequenceTracker tracker;
    LatencyStats latency_stats;

    for (uint32_t i = 0; i < record_count; ++i) {
        uint8_t rec_hdr[12];
        ifs.read(reinterpret_cast<char*>(rec_hdr), sizeof(rec_hdr));
        if (!ifs.good()) {
            res.execution_status = "FAILED";
            res.status = Status::Error(StatusCode::kIoError, "Truncated fixture record header");
            return res;
        }
        const uint64_t recv_ts_ns = read_u64_le(rec_hdr);
        const uint32_t frame_len = read_u32_le(rec_hdr + 8);
        if (frame_len > kMaxPacketBytes) {
            res.execution_status = "FAILED";
            res.status = Status::Error(StatusCode::kInvalidConfig, "Fixture frame exceeds kMaxPacketBytes");
            return res;
        }
        ReceivedFrame rf{};
        rf.recv_ts_ns = recv_ts_ns;
        rf.length = frame_len;
        ifs.read(reinterpret_cast<char*>(rf.data.data()), static_cast<std::streamsize>(frame_len));
        if (!ifs.good()) {
            res.execution_status = "FAILED";
            res.status = Status::Error(StatusCode::kIoError, "Truncated fixture frame payload");
            return res;
        }

        if (!ring.try_push(std::move(rf))) {
            tracker.on_ring_drop(1);
            continue;
        }

        ReceivedFrame popped{};
        if (ring.try_pop(popped)) {
            DecodedPacket pkt{};
            const DecodeError derr = decode_packet(
                std::span<const uint8_t>(popped.data.data(), popped.length), pkt);
            if (derr != DecodeError::kOk) {
                tracker.on_decode_error(derr);
                continue;
            }
            const PacketObservation obs = tracker.on_packet(pkt.header);
            if (obs == PacketObservation::kInOrder ||
                obs == PacketObservation::kGapDetected ||
                obs == PacketObservation::kOutOfOrder) {
                const bool is_warmup = (pkt.header.flags & kFlagWarmup) != 0;
                latency_stats.record_sample(pkt.header.send_ts_ns, popped.recv_ts_ns, is_warmup);
            }
        }
    }

    res.execution_status = "FIXTURE_VERIFICATION";
    res.status = Status::Ok();
    res.actual_packets_sent = expected_total_sent;
    res.sequence = tracker.counters();
    res.inferred_total_lost = res.sequence.lost_against_expected(expected_total_sent);
    res.loss_ratio = (expected_total_sent > 0)
                         ? static_cast<double>(res.inferred_total_lost) / static_cast<double>(expected_total_sent)
                         : 0.0;
    res.sequence_accounting_verified =
        res.sequence.accounting_invariant_holds() &&
        (res.sequence.unique_received + res.inferred_total_lost == expected_total_sent);
    res.latency = latency_stats.summarize();
    return res;
}

} // namespace kbnl
