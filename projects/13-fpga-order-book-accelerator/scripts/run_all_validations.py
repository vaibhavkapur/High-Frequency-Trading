#!/usr/bin/env python3
"""End-to-end build, validation, benchmark, and raw report generator."""

import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.path.join(ROOT, "build")
REPORTS = os.path.join(ROOT, "reports")


def run_cmd(cmd, cwd=ROOT, check=True):
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        print(f"[ERROR] Command failed ({proc.returncode}): {' '.join(cmd)}", file=sys.stderr)
        print(proc.stdout, file=sys.stderr)
        print(proc.stderr, file=sys.stderr)
        sys.exit(proc.returncode)
    return proc


def tool_version(name, args):
    path = shutil.which(name)
    if not path:
        return {"installed": False, "path": None, "version": None}
    proc = subprocess.run([path] + args, capture_output=True, text=True)
    line = (proc.stdout or proc.stderr).strip().splitlines()
    return {"installed": True, "path": path, "version": line[0] if line else ""}


def main() -> int:
    os.makedirs(REPORTS, exist_ok=True)

    # 1. Record runtime environment & tool availability
    env_info = {
        "schema_version": "fpga-ob.v1",
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "tools": {
            "cmake": tool_version("cmake", ["--version"]),
            "clang++": tool_version("clang++", ["--version"]),
            "g++": tool_version("g++", ["--version"]),
            "verilator": tool_version("verilator", ["--version"]),
            "yosys": tool_version("yosys", ["-V"]),
            "cocotb-config": tool_version("cocotb-config", ["--version"]),
        },
    }

    # 2. Configure and build C++20 Release
    run_cmd(["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"])
    run_cmd(["cmake", "--build", "build", "-j"])

    # 3. Run CTest (unit, integration, 25k differential, Python reference check)
    ctest_res = run_cmd(["ctest", "--test-dir", "build", "--output-on-failure"])
    print(ctest_res.stdout.strip())

    # 4. Run raw artifact generators (scenarios, 25k differential, 100k benchmark)
    run_cmd([os.path.join(BUILD, "test_integration"), "--out", os.path.join(REPORTS, "scenarios.json")])
    run_cmd(
        [
            os.path.join(BUILD, "test_differential"),
            "--events",
            "25000",
            "--mixed",
            "10000",
            "--seed",
            "20261127",
            "--out",
            os.path.join(REPORTS, "differential_10k.json"),
        ]
    )
    run_cmd(
        [
            os.path.join(BUILD, "bench_latency"),
            "--events",
            "100000",
            "--warmup",
            "2000",
            "--seed",
            "20261127",
            "--out",
            os.path.join(REPORTS, "benchmark_latency.json"),
        ]
    )

    # 5. Run demo CLI and capture sample output + verify operational missing-config failure
    demo_ok = run_cmd(
        [
            os.path.join(BUILD, "fpga_ob_cli"),
            "--mode",
            "demo",
            "--fixture",
            "tests/fixtures/deterministic_basic.json",
            "--verbose",
        ]
    )
    with open(os.path.join(REPORTS, "demo_output.json"), "w", encoding="utf-8") as f:
        f.write(demo_ok.stdout)

    missing_cfg = run_cmd(
        [os.path.join(BUILD, "fpga_ob_cli"), "--mode", "demo", "--fixture", "tests/fixtures/does_not_exist.json"],
        check=False,
    )
    if missing_cfg.returncode == 0:
        print("[ERROR] Missing fixture did not return non-zero exit code!", file=sys.stderr)
        return 1

    # 6. Optional EDA checks (explicitly mark NOT_RUN if unavailable)
    eda_status = {
        "verilator_rtl_sim": "NOT_RUN (verilator not installed)",
        "cocotb_rtl_testbench": "NOT_RUN (verilator/cocotb not installed)",
        "yosys_synthesis_estimate": "NOT_RUN (yosys not installed)",
        "physical_fpga_hardware_timing": "NOT_RUN (no FPGA target hardware attached)",
    }

    if env_info["tools"]["verilator"]["installed"]:
        v_res = run_cmd(["make", "sim-verilator"], check=False)
        eda_status["verilator_rtl_sim"] = "PASS" if v_res.returncode == 0 else f"FAIL ({v_res.returncode})"

    if env_info["tools"]["verilator"]["installed"] and env_info["tools"]["cocotb-config"]["installed"]:
        c_res = run_cmd(["make", "sim-cocotb"], check=False)
        eda_status["cocotb_rtl_testbench"] = "PASS" if c_res.returncode == 0 else f"FAIL ({c_res.returncode})"

    if env_info["tools"]["yosys"]["installed"]:
        y_res = run_cmd(["make", "synth-yosys"], check=False)
        eda_status["yosys_synthesis_estimate"] = "PASS" if y_res.returncode == 0 else f"FAIL ({y_res.returncode})"

    env_info["operational_checks"] = {
        "missing_config_exit_code": missing_cfg.returncode,
        "missing_config_stderr": missing_cfg.stderr.strip(),
        "demo_fixture_exit_code": demo_ok.returncode,
    }
    env_info["optional_eda_status"] = eda_status

    with open(os.path.join(REPORTS, "environment.json"), "w", encoding="utf-8") as f:
        json.dump(env_info, f, indent=2)

    print("[PASS] End-to-end validation complete. Raw reports written to reports/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
