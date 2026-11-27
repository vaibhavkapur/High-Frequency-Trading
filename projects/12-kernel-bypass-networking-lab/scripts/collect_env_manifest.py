#!/usr/bin/env python3
"""Collects reproducible hardware, OS, compiler, clock, and NIC/capability manifest."""

import json
import os
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone


def run_cmd(args):
    try:
        out = subprocess.check_output(args, stderr=subprocess.STDOUT, text=True, timeout=5)
        return out.strip()
    except Exception:
        return None


def detect_cpu_model():
    sysname = platform.system()
    if sysname == "Darwin":
        return run_cmd(["sysctl", "-n", "machdep.cpu.brand_string"]) or platform.processor()
    if sysname == "Linux":
        try:
            with open("/proc/cpuinfo", "r", encoding="utf-8") as f:
                for line in f:
                    if line.lower().startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return platform.processor() or "unknown"


def detect_memory_bytes():
    sysname = platform.system()
    if sysname == "Darwin":
        val = run_cmd(["sysctl", "-n", "hw.memsize"])
        return int(val) if val and val.isdigit() else None
    if sysname == "Linux":
        try:
            with open("/proc/meminfo", "r", encoding="utf-8") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        kb = int(line.split()[1])
                        return kb * 1024
        except OSError:
            pass
    return None


def detect_interfaces():
    sysname = platform.system()
    if sysname == "Linux" and os.path.isdir("/sys/class/net"):
        return sorted(os.listdir("/sys/class/net"))
    if sysname == "Darwin":
        out = run_cmd(["ifconfig", "-l"])
        if out:
            return out.split()
    return []


def main():
    output_path = sys.argv[1] if len(sys.argv) > 1 else "reports/environment_manifest.json"
    compiler_path = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    compiler_ver = run_cmd([compiler_path, "--version"]) if compiler_path else None
    if compiler_ver:
        compiler_ver = compiler_ver.splitlines()[0]

    cmake_ver = run_cmd(["cmake", "--version"])
    if cmake_ver:
        cmake_ver = cmake_ver.splitlines()[0]

    sysname = platform.system()
    manifest = {
        "schema_version": 1,
        "collected_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "host": {
            "os_name": sysname,
            "os_release": platform.release(),
            "os_version": platform.version(),
            "architecture": platform.machine(),
            "cpu_model": detect_cpu_model(),
            "logical_cpus": os.cpu_count(),
            "memory_bytes": detect_memory_bytes(),
        },
        "toolchain": {
            "cmake": cmake_ver,
            "cxx_compiler": compiler_ver,
            "build_type": os.environ.get("KBNL_BUILD_TYPE", "Release"),
            "cxx_standard": "C++20",
            "af_xdp_enabled_flag": os.environ.get("KBNL_ENABLE_AF_XDP", "OFF"),
            "dpdk_enabled_flag": os.environ.get("KBNL_ENABLE_DPDK", "OFF"),
        },
        "networking": {
            "interfaces": detect_interfaces(),
            "euid": os.geteuid() if hasattr(os, "geteuid") else None,
            "af_xdp_platform_supported": sysname == "Linux",
            "dpdk_platform_supported": sysname == "Linux",
            "recvmmsg_platform_supported": sysname == "Linux",
            "strict_thread_pinning_supported": sysname == "Linux",
        },
        "clock_policy": {
            "source": "CLOCK_MONOTONIC_RAW",
            "domain": "same_host_only",
            "notes": "Cross-host latency requires hardware timestamping (SO_TIMESTAMPING / PTP) which is outside loopback scope.",
        },
    }

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"Wrote environment manifest to {output_path}")


if __name__ == "__main__":
    main()
