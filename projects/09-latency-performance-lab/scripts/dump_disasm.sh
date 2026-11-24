#!/usr/bin/env bash
set -euo pipefail

BINARY="${1:-build/benchmarks/bench_suite}"
OUT_FILE="${2:-results/disasm_kernels.txt}"

if [[ ! -f "${BINARY}" ]]; then
  echo "Binary not found: ${BINARY}" >&2
  exit 1
fi

mkdir -p "$(dirname "${OUT_FILE}")"

SYMBOLS=(
  "branch_filter_natural_if"
  "branch_filter_forced_branch"
  "branch_filter_branchless"
  "scan_sequential_aos"
  "scan_sequential_soa"
)

{
  echo "# Disassembly of hot microbenchmark kernels"
  echo "# Binary: ${BINARY}"
  echo "# Host: $(uname -s) $(uname -m)"
  echo ""

  for sym in "${SYMBOLS[@]}"; do
    echo "================================================================"
    echo "SYMBOL PATTERN: ${sym}"
    echo "================================================================"
    if command -v objdump >/dev/null 2>&1; then
      # Works on both macOS (llvm-objdump via /usr/bin/objdump) and Linux GNU objdump
      objdump -d -C "${BINARY}" | awk -v pat="${sym}" '
        /^$/ { if (printing) { print ""; printing=0 } }
        /<.*>/ && $0 ~ pat { printing=1 }
        printing { print }
      ' | head -n 90
    elif command -v otool >/dev/null 2>&1; then
      otool -tV "${BINARY}" | awk -v pat="${sym}" '
        $0 ~ pat && /:$/ { printing=1; print; next }
        printing && /^[A-Za-z_0-9].*:$/ { printing=0 }
        printing { print }
      ' | head -n 90
    else
      echo "Neither objdump nor otool available."
    fi
    echo ""
  done
} > "${OUT_FILE}"

echo "Wrote disassembly to ${OUT_FILE}"
