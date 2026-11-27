BUILD_DIR := build
RTL_SRCS  := rtl/order_book_pkg.sv rtl/bbo_tree.sv rtl/price_level_bank.sv rtl/order_book_top.sv

.PHONY: all build test bench demo validate sim-verilator sim-cocotb synth-yosys clean

all: build test

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

bench: build
	./$(BUILD_DIR)/bench_latency --events 100000 --seed 20261127 --out reports/benchmark_latency.json

demo: build
	./$(BUILD_DIR)/fpga_ob_cli --mode demo --fixture tests/fixtures/deterministic_basic.json

validate:
	python3 scripts/run_all_validations.py

sim-verilator:
	@if command -v verilator >/dev/null 2>&1; then \
		verilator --cc --exe --build --Wall -j 0 \
			--top-module order_book_top \
			-Mdir $(BUILD_DIR)/obj_dir \
			-CFLAGS "-std=c++20 -I$(CURDIR)/include" \
			$(RTL_SRCS) sim/verilator_main.cpp src/contracts.cpp src/reference_book.cpp; \
		./$(BUILD_DIR)/obj_dir/Vorder_book_top --events 10000 --seed 20261127; \
	else \
		echo "SKIPPED: verilator is not installed in this environment."; \
	fi

sim-cocotb:
	@if command -v verilator >/dev/null 2>&1 && python3 -c "import cocotb" >/dev/null 2>&1; then \
		$(MAKE) -C tests/cocotb SIM=verilator; \
	else \
		echo "SKIPPED: verilator and/or cocotb is not installed in this environment."; \
	fi

synth-yosys:
	@if command -v yosys >/dev/null 2>&1; then \
		yosys -s scripts/synth_yosys.ys | tee reports/yosys_synth.log; \
	else \
		echo "SKIPPED: yosys is not installed in this environment."; \
	fi

clean:
	rm -rf $(BUILD_DIR)
