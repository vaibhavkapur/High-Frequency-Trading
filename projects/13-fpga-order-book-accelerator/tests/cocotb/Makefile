# cocotb RTL Testbench Makefile (executed via `make sim-cocotb` when Verilator + cocotb are installed)
SIM ?= verilator
TOPLEVEL_LANG ?= verilog

VERILOG_SOURCES += $(PWD)/../../rtl/order_book_pkg.sv
VERILOG_SOURCES += $(PWD)/../../rtl/bbo_tree.sv
VERILOG_SOURCES += $(PWD)/../../rtl/price_level_bank.sv
VERILOG_SOURCES += $(PWD)/../../rtl/order_book_top.sv

TOPLEVEL = order_book_top
MODULE   = test_order_book

EXTRA_ARGS += --Wall -Wno-DECLFILENAME

include $(shell cocotb-config --makefiles)/Makefile.sim
