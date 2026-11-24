# Milestone M4: Deep-Dive Analysis of Surprising Results (with Disassembly)

All timing, hardware counter, and disassembly excerpts below were captured directly on **Apple M5 (arm64, 128 B cache lines)** with **AppleClang 21.0.0 (`-O3 -DNDEBUG`)** from [results/microbenchmarks_summary.csv](../results/microbenchmarks_summary.csv) and [results/disasm_kernels.txt](../results/disasm_kernels.txt).

---

## Finding 1: Why `if (p[i] >= threshold) sum += p[i];` Showed **Zero** Branch Misprediction Penalty on Unsorted Data

### The Hypothesis vs. The Surprise

In textbook microbenchmark lore, filtering an unsorted array with an `if` statement is expected to suffer a large branch-misprediction penalty compared to a sorted array. Yet when we timed `branch_filter_natural_if` across 65,536 elements (`7 reps × 25 samples`):

| Kernel | Input Order | p50 (ns/op) | Cyc/op | Ins/op | IPC |
|---|---|---:|---:|---:|---:|
| `branch_filter_natural_if` | Sorted | **0.06** | **0.25** | **1.26** | **4.98** |
| `branch_filter_natural_if` | Unsorted | **0.06** | **0.25** | **1.26** | **4.99** |
| `branch_filter_branchless` | Sorted | **0.06** | **0.25** | **1.26** | **4.98** |
| `branch_filter_branchless` | Unsorted | **0.06** | **0.25** | **1.26** | **4.98** |
| `branch_filter_forced_branch` | Sorted | **0.23** | **1.01** | **6.00** | **5.96** |
| `branch_filter_forced_branch` | Unsorted | **2.33** | **10.10** | **6.02** | **0.60** |

`natural_if` ran at **17,282 Mops/s (0.25 cycles/element!)** on *both* sorted and unsorted data — completely indistinguishable from our hand-written `branchless_mask` kernel.

### Disassembly Proof (`objdump -d -C build/benchmarks/bench_suite`)

Inspecting the actual emitted ARM64 machine code in [results/disasm_kernels.txt](../results/disasm_kernels.txt) immediately explains why:

**1. `lpl::branch_filter_natural_if` (hot loop, lines 39–58):**

```asm
1000171e0: ad7f4991  ldp       q17, q18, [x12, #-0x20]   ; load 8 x uint32
1000171e4: acc25193  ldp       q19, q20, [x12], #0x40    ; load next 8 x uint32 (16 elements/iter)
1000171e8: 6ea03e35  cmhs.4s   v21, v17, v0              ; SIMD >= mask (no branch!)
1000171ec: 4e311eb1  and.16b   v17, v21, v17             ; zero out lanes < threshold
1000171f0: 6ea03e55  cmhs.4s   v21, v18, v0
1000171f4: 4e321eb2  and.16b   v18, v21, v18
1000171f8: 6ea03e75  cmhs.4s   v21, v19, v0
1000171fc: 4e331eb3  and.16b   v19, v21, v19
100017200: 6ea03e95  cmhs.4s   v21, v20, v0
100017204: 4e341eb4  and.16b   v20, v21, v20
100017208: 6eb11042  uaddw2.2d v2, v2, v17               ; widen & accumulate 64-bit sums
10001720c: 2eb11021  uaddw.2d  v1, v1, v17
...
100017228: f10041ad  subs      x13, x13, #0x10           ; 16 elements per loop iteration
10001722c: 54fffda1  b.ne      0x1000171e0
```

Even the scalar tail (lines 93–96) was if-converted by Clang into conditional select (`csel`):

```asm
1000172b8: b840450a  ldr   w10, [x8], #0x4
1000172bc: 6b01015f  cmp   w10, w1
1000172c0: 1a8a33ea  csel  w10, wzr, w10, lo             ; branchless conditional select!
1000172c4: 8b0a0000  add   x0, x0, x10
```

Comparing this against `lpl::branch_filter_branchless` (lines 157–176 of `disasm_kernels.txt`), Clang generated the **exact same instruction sequence** (`ldp` -> `cmhs.4s` -> `and.16b` -> `uaddw`/`uaddw2`). Writing a "branchy" `if` microbenchmark without inspecting disassembly would have silently measured SIMD branchless execution instead of branch prediction!

**2. `lpl::branch_filter_forced_branch` (hot loop, lines 111–117):**

By placing an opaque `asm volatile("" : "+r"(sum))` barrier inside the taken branch, we prevented if-conversion and forced a genuine conditional branch (`b.lo`):

```asm
1000174b0: f1000529  subs  x9, x9, #0x1
1000174b4: 540000c0  b.eq  0x1000174cc
1000174b8: b840450a  ldr   w10, [x8], #0x4
1000174bc: 6b01015f  cmp   w10, w1
1000174c0: 54ffff83  b.lo  0x1000174b0                   ; real conditional branch
1000174c4: 8b0a0000  add   x0, x0, x10
1000174c8: 17fffffa  b     0x1000174b0
```

Once a real conditional branch exists, the hardware counter story becomes unmistakable:
- Both sorted and unsorted execute **6.00 retired instructions/op**.
- On **sorted** data, the branch predictor achieves near-100% accuracy: **1.01 cycles/op** (**IPC = 5.96**).
- On **unsorted** data (~50% random taken/not-taken), pipeline flushes explode latency to **10.10 cycles/op** (**IPC collapses 10x to 0.60**), yielding a **10.1x wall-time slowdown** (0.23 ns -> 2.33 ns) despite identical instruction count.

**Takeaway:** Never trust a microbenchmark's C++ source alone to tell you what the CPU executed — correlate latency with **instructions/op, cycles/op, IPC, and disassembly**.

---

## Finding 2: `alignas(64)` Still Suffers False Sharing on Apple Silicon

Many low-latency C++ codebases hardcode `alignas(64)` assuming x86-64 cache lines. On Apple Silicon (`hw.cachelinesize = 128` bytes), two `alignas(64)` atomics still share the same 128-byte coherence granule:

| Padding | p50 (ns/op) | p99 (ns/op) | Cyc/op | Ins/op | IPC | Rep CV% | Speedup vs 8B |
|---|---:|---:|---:|---:|---:|---:|---:|
| `align_8B_packed` | 2.51 | 3.49 | 35.96 | 6.41 | **0.18** | 7.90% | 1.00x |
| `align_64B_padded` | 0.58 | 2.17 | 10.32 | 6.32 | **0.61** | **55.03%** | 4.33x |
| `align_128B_padded` | **0.42** | **0.52** | **7.11** | 6.28 | **0.88** | **0.27%** | **5.92x** |

Notice two things:
1. All three execute the exact same **~6.3 instructions/op** — the entire 5.92x speedup comes from eliminating coherence stalls (cycles/op dropping from 35.96 -> 10.32 -> 7.11).
2. `align_64B_padded` exhibits **55.03% cross-run CV** and **4.17x worse p99** (2.17 ns vs 0.52 ns) than `align_128B_padded`, because pairs of threads randomly land on the same 128 B line depending on OS placement. Padding to **128 B** (which we applied to `SpscRingBuffer` as well) collapses variance to **0.27% CV** and gives portable false-sharing immunity on both x86-64 (adjacent-line prefetcher) and ARM64 Apple Silicon.

---

## Finding 3: Why AoS Beats SoA on Random Multi-Field Access (and Why Hash Maps Lose on Order-Book Top-K)

1. **AoS vs SoA is access-pattern dependent, not universally "SoA > AoS":**
   - On sequential hot-field scans, `scan_sequential_soa` vectorizes with NEON (`ldp q16, q17`, `cmeq.8h`, `and.16b` in `disasm_kernels.txt` lines 321–336) and skips 50 bytes/record of cold fields, cutting cycles/op from **2.45 -> 1.34** (**IPC 2.76 -> 5.38**).
   - On **random multi-field access** (`price`, `qty`, `side`, `order_id`), **AoS beats SoA by 1.89x** (0.99 ns vs 1.87 ns; 3.96 vs 8.09 cycles/op; **IPC 2.28 vs 0.99**), because AoS fetches all 4 fields in a single cache-line miss while SoA touches 4 separate column arrays.
2. **Hash maps lose badly when used directly for ordered book queries:**
   - At depth 512, `sorted_vector` runs in **11.04 ns/op (113 insns/op)** vs `std::map` at **14.11 ns/op (166 insns/op)**, whereas `std::unordered_map` takes **62.07 ns/op (814 insns/op)** and `flat_hash_map` takes **149.35 ns/op (2,114 insns/op)** because `top_k_volume(5)` and post-cancel BBO rediscovery must scan the unordered table.
   - Pairing the right container with each access pattern — **open-addressing `FlatHashMap` for unordered O(1) order-ID lookup** + **contiguous sorted price-level vector + `ObjectPool` intrusive queues for ordered matching** — is why Engine B achieved **1.88x lower p50, 2.04x lower p99, and 2.52x lower p99.9** in the integrated matching-engine A/B test.
