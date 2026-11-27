#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fpga_ob/contracts.hpp"
#include "fpga_ob/reference_book.hpp"
#include "fpga_ob/rtl_model.hpp"

namespace {

void print_usage() {
    std::cerr << "Usage: fpga_ob_cli --mode <demo|replay> --fixture <path.json> "
                 "[--binary-in <path.bin>] [--binary-out <path.bin>] [--verbose]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "demo";
    std::string fixture_path;
    std::string binary_in;
    std::string binary_out;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--fixture" && i + 1 < argc) {
            fixture_path = argv[++i];
        } else if (arg == "--binary-in" && i + 1 < argc) {
            binary_in = argv[++i];
        } else if (arg == "--binary-out" && i + 1 < argc) {
            binary_out = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "{\"error\":\"INVALID_ARGUMENT\",\"argument\":\"" << arg << "\"}\n";
            print_usage();
            return 2;
        }
    }

    if (mode != "demo" && mode != "replay") {
        std::cerr << "{\"error\":\"UNSUPPORTED_MODE\",\"mode\":\"" << mode << "\"}\n";
        return 2;
    }

    if (fixture_path.empty() && binary_in.empty()) {
        std::cerr << "{\"error\":\"CONFIG_MISSING\",\"message\":\"Specify --fixture <path.json> or --binary-in <path.bin>\"}\n";
        return 2;
    }

    try {
        fpga_ob::Config cfg{};
        std::string fixture_name = "binary_stream";
        std::vector<fpga_ob::EventPacket> events;

        if (!fixture_path.empty()) {
            auto bundle  = fpga_ob::load_events_json(fixture_path);
            cfg          = bundle.config;
            fixture_name = bundle.name.empty() ? fixture_path : bundle.name;
            events       = std::move(bundle.events);
        } else {
            events = fpga_ob::read_binary_stream(binary_in);
        }

        if (!binary_out.empty()) {
            fpga_ob::write_binary_stream(binary_out, events);
        }

        fpga_ob::PipelinedOrderBookAccelerator dut(cfg);
        fpga_ob::ReferenceOrderBook ref(cfg);

        // Reset pulse
        dut.tick(fpga_ob::TickInput{.rst_n = false});

        std::vector<fpga_ob::BboOutputPacket> dut_outputs;
        std::vector<fpga_ob::BboOutputPacket> ref_outputs;
        dut_outputs.reserve(events.size());
        ref_outputs.reserve(events.size());

        for (const auto& ev : events) {
            ref_outputs.push_back(ref.apply_single(ev));
            auto out = dut.tick(fpga_ob::TickInput{
                .rst_n      = true,
                .in_valid_0 = true,
                .in_event_0 = ev,
                .in_valid_1 = false,
                .out_ready  = true,
            });
            if (out.out_valid) {
                dut_outputs.push_back(out.out_bbo);
            }
        }

        // Flush remaining pipeline stages (3-cycle depth)
        for (uint32_t i = 0; i < fpga_ob::PipelinedOrderBookAccelerator::kPipelineLatencyCycles + 1; ++i) {
            auto out = dut.tick(fpga_ob::TickInput{.rst_n = true, .in_valid_0 = false, .out_ready = true});
            if (out.out_valid) {
                dut_outputs.push_back(out.out_bbo);
            }
        }

        size_t mismatches = 0;
        if (dut_outputs.size() != ref_outputs.size()) {
            mismatches = std::max(dut_outputs.size(), ref_outputs.size());
        } else {
            for (size_t i = 0; i < dut_outputs.size(); ++i) {
                if (!(dut_outputs[i] == ref_outputs[i])) {
                    ++mismatches;
                }
            }
        }

        std::cout << "{"
                  << "\"schema_version\":\"fpga-ob.v1\","
                  << "\"mode\":\"" << mode << "\","
                  << "\"fixture\":\"" << fixture_name << "\","
                  << "\"events_processed\":" << events.size() << ","
                  << "\"reference_mismatches\":" << mismatches << ","
                  << "\"diagnostics\":" << dut.diagnostics().to_json() << ","
                  << "\"final_bbo\":" << (dut_outputs.empty() ? "null" : fpga_ob::to_json(dut_outputs.back()));

        if (verbose) {
            std::cout << ",\"outputs\":[";
            for (size_t i = 0; i < dut_outputs.size(); ++i) {
                if (i > 0) std::cout << ",";
                std::cout << fpga_ob::to_json(dut_outputs[i]);
            }
            std::cout << "]";
        }
        std::cout << "}\n";

        return (mismatches == 0) ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "{\"error\":\"CONFIG_MISSING_OR_INVALID\",\"message\":\"" << ex.what() << "\"}\n";
        return 2;
    }
}
