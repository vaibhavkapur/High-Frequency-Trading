#!/usr/bin/env python3
"""Compute comparison tables, A/B speedups, variance stats, and SVG plots from raw CSV outputs.

All numbers in the generated report are derived strictly from captured CSV/JSON artifacts.
"""

import argparse
import csv
import json
import os
from collections import defaultdict


def load_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def fmt(val, digits=2, suffix=""):
    try:
        v = float(val)
    except (TypeError, ValueError):
        return "n/a"
    if v < 0:
        return "n/a"
    return f"{v:.{digits}f}{suffix}"


def generate_throughput_latency_svg(sweep_rows, svg_path):
    if not sweep_rows:
        return

    by_variant = defaultdict(list)
    for row in sweep_rows:
        by_variant[row["variant"]].append(
            (
                float(row["achieved_rate_kops"]),
                float(row["p50_ns"]),
                float(row["p99_ns"]),
            )
        )
    for v in by_variant:
        by_variant[v].sort(key=lambda t: t[0])

    all_x = [pt[0] for pts in by_variant.values() for pt in pts]
    all_y = [pt[2] for pts in by_variant.values() for pt in pts]
    max_x = max(all_x) * 1.08 if all_x else 1.0
    max_y = max(all_y) * 1.15 if all_y else 1.0

    width, height = 820, 440
    left, right, top, bottom = 78, 30, 50, 62
    plot_w = width - left - right
    plot_h = height - top - bottom

    def sx(x):
        return left + (x / max_x) * plot_w

    def sy(y):
        return top + plot_h - (y / max_y) * plot_h

    colors = {
        "engine_A_baseline": "#d9534f",
        "engine_B_pooled_flat": "#2b7de9",
    }

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{width/2:.1f}" y="26" text-anchor="middle" font-family="sans-serif" font-size="16" font-weight="bold" fill="#222">Matching Engine A/B: Throughput vs Latency (p50 solid, p99 dashed)</text>',
    ]

    # Gridlines & axes
    for i in range(6):
        gx = left + (plot_w * i) / 5.0
        xv = (max_x * i) / 5.0
        lines.append(f'<line x1="{gx:.1f}" y1="{top}" x2="{gx:.1f}" y2="{top+plot_h}" stroke="#e8e8e8"/>')
        lines.append(f'<text x="{gx:.1f}" y="{top+plot_h+20}" text-anchor="middle" font-family="sans-serif" font-size="11" fill="#555">{xv:.0f}</text>')

        gy = top + plot_h - (plot_h * i) / 5.0
        yv = (max_y * i) / 5.0
        lines.append(f'<line x1="{left}" y1="{gy:.1f}" x2="{left+plot_w}" y2="{gy:.1f}" stroke="#e8e8e8"/>')
        lines.append(f'<text x="{left-10}" y="{gy+4:.1f}" text-anchor="end" font-family="sans-serif" font-size="11" fill="#555">{yv:.0f}</text>')

    lines.append(f'<line x1="{left}" y1="{top+plot_h}" x2="{left+plot_w}" y2="{top+plot_h}" stroke="#333" stroke-width="1.5"/>')
    lines.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}" stroke="#333" stroke-width="1.5"/>')
    lines.append(f'<text x="{left+plot_w/2:.1f}" y="{height-16}" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#333">Achieved Throughput (k ops/sec)</text>')
    lines.append(f'<text x="20" y="{top+plot_h/2:.1f}" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#333" transform="rotate(-90 20 {top+plot_h/2:.1f})">Latency (ns/order)</text>')

    for variant, pts in sorted(by_variant.items()):
        color = colors.get(variant, "#333333")
        p50_poly = " ".join(f"{sx(x):.1f},{sy(p50):.1f}" for x, p50, _ in pts)
        p99_poly = " ".join(f"{sx(x):.1f},{sy(p99):.1f}" for x, _, p99 in pts)
        lines.append(f'<polyline fill="none" stroke="{color}" stroke-width="2.5" points="{p50_poly}"/>')
        lines.append(f'<polyline fill="none" stroke="{color}" stroke-width="2" stroke-dasharray="6,4" points="{p99_poly}"/>')
        for x, p50, p99 in pts:
            lines.append(f'<circle cx="{sx(x):.1f}" cy="{sy(p50):.1f}" r="3.5" fill="{color}"/>')
            lines.append(f'<rect x="{sx(x)-3:.1f}" y="{sy(p99)-3:.1f}" width="6" height="6" fill="{color}" opacity="0.7"/>')

    # Legend
    lx, ly = left + 18, top + 14
    lines.append(f'<rect x="{lx-8}" y="{ly-12}" width="265" height="52" fill="#ffffff" fill-opacity="0.9" stroke="#ccc"/>')
    for idx, variant in enumerate(sorted(by_variant.keys())):
        color = colors.get(variant, "#333333")
        yy = ly + idx * 22
        lines.append(f'<line x1="{lx}" y1="{yy}" x2="{lx+24}" y2="{yy}" stroke="{color}" stroke-width="2.5"/>')
        lines.append(f'<text x="{lx+32}" y="{yy+4}" font-family="sans-serif" font-size="11" fill="#222">{variant} (p50 / p99)</text>')

    lines.append("</svg>\n")
    with open(svg_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def build_report(results_dir):
    env_path = os.path.join(results_dir, "environment.json")
    micro_rows = load_csv(os.path.join(results_dir, "microbenchmarks_summary.csv"))
    ab_rows = load_csv(os.path.join(results_dir, "matching_ab_summary.csv"))
    sweep_rows = load_csv(os.path.join(results_dir, "matching_throughput_latency.csv"))

    env = {}
    if os.path.exists(env_path):
        with open(env_path, encoding="utf-8") as f:
            env = json.load(f)

    out = []
    out.append("# Automated Benchmark Comparison Report")
    out.append("")
    out.append("> Generated programmatically by `scripts/analyze_results.py` directly from captured CSV/JSON artifacts.")
    out.append("")

    if env:
        out.append("## Hardware & Software Baseline")
        out.append("")
        out.append("| Field | Value |")
        out.append("|---|---|")
        topo = f"{env.get('cpu_model', 'unknown')} ({env.get('architecture', '')}), {env.get('physical_cores', 0)} physical / {env.get('logical_cores', 0)} logical"
        if env.get("perf_cores", 0) or env.get("eff_cores", 0):
            topo += f" ({env['perf_cores']}P + {env['eff_cores']}E)"
        topo += f", cache line {env.get('cache_line_bytes', 0)} B"
        out.append(f"| CPU / cores / topology | {topo} |")
        out.append(
            f"| Caches (L1d / L2 / L3) | {env.get('l1d_cache_bytes', 0)//1024} KiB / "
            f"{env.get('l2_cache_bytes', 0)//1024} KiB / {env.get('l3_cache_bytes', 0)//1024} KiB |"
        )
        out.append(f"| OS / kernel | {env.get('os_name', '')} (kernel {env.get('kernel_release', '')}) |")
        out.append(
            f"| Compiler / version / flags | {env.get('compiler_id', '')} {env.get('compiler_version', '')} "
            f"({env.get('build_type', '')}, `{env.get('cxx_flags', '')}`, `-march=native`: {env.get('march_native', False)}) |"
        )
        out.append(f"| Governor / turbo | {env.get('cpu_governor', '')} / {env.get('turbo_status', '')} |")
        out.append(f"| Median clock overhead | {fmt(env.get('clock_overhead_ns', 0), 2, ' ns')} |")
        out.append(f"| Captured UTC | {env.get('timestamp_utc', '')} |")
        out.append("")

    if micro_rows:
        out.append("## Microbenchmark Results")
        out.append("")
        groups = defaultdict(list)
        for r in micro_rows:
            groups[(r["suite"], r["name"])].append(r)

        out.append("| Suite / Workload | Variant | p50 (ns/op) | p99 (ns/op) | Mops/s | Cyc/op | Ins/op | IPC | CPU/Wall | Rep CV% | vs Baseline |")
        out.append("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")

        for (suite, name), rows in groups.items():
            base_p50 = float(rows[0]["p50_ns"])
            for idx, r in enumerate(rows):
                p50 = float(r["p50_ns"])
                ratio = f"{base_p50 / p50:.2f}x" if (idx > 0 and p50 > 0) else "1.00x (base)"
                out.append(
                    f"| `{suite}/{name}` | `{r['variant']}` | {fmt(r['p50_ns'])} | {fmt(r['p99_ns'])} | "
                    f"{fmt(r['throughput_mops'])} | {fmt(r['cycles_per_op'])} | {fmt(r['instructions_per_op'])} | "
                    f"{fmt(r['ipc'])} | {fmt(r['cpu_to_wall_ratio'])} | {fmt(r['rep_p50_cv_pct'], 2, '%')} | **{ratio}** |"
                )
        out.append("")

    if len(ab_rows) >= 2:
        a, b = ab_rows[0], ab_rows[1]
        p50_a, p50_b = float(a["p50_ns"]), float(b["p50_ns"])
        p99_a, p99_b = float(a["p99_ns"]), float(b["p99_ns"])
        p999_a, p999_b = float(a["p999_ns"]), float(b["p999_ns"])
        tp_a, tp_b = float(a["throughput_mops"]), float(b["throughput_mops"])

        out.append("## Integrated Matching Engine A/B Comparison")
        out.append("")
        out.append("| Variant | p50 (ns/order) | p99 (ns/order) | p99.9 (ns/order) | Throughput (Mops/s) | Cyc/op | Ins/op | IPC | Rep CV% | Checksum |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in (a, b):
            out.append(
                f"| `{r['variant']}` | {fmt(r['p50_ns'])} | {fmt(r['p99_ns'])} | {fmt(r['p999_ns'])} | "
                f"{fmt(r['throughput_mops'])} | {fmt(r['cycles_per_op'])} | {fmt(r['instructions_per_op'])} | "
                f"{fmt(r['ipc'])} | {fmt(r['rep_p50_cv_pct'], 2, '%')} | `{r['checksum']}` |"
            )
        out.append("")
        out.append(
            f"- **Computed A/B Speedup (B vs A):** "
            f"**{p50_a / p50_b:.2f}x** lower p50 latency ({p50_a:.2f} ns -> {p50_b:.2f} ns), "
            f"**{p99_a / p99_b:.2f}x** lower p99 latency ({p99_a:.2f} ns -> {p99_b:.2f} ns), "
            f"**{p999_a / p999_b:.2f}x** lower p99.9 latency, "
            f"**{tp_b / tp_a:.2f}x** higher median throughput ({tp_a:.2f} -> {tp_b:.2f} Mops/s)."
        )
        out.append(
            f"- **Equivalence Check:** Checksums identical (`{a['checksum'] == b['checksum']}`)."
        )
        out.append("")

    if sweep_rows:
        out.append("## Throughput-vs-Latency Load Sweep")
        out.append("")
        out.append("| Variant | Target Rate (k ops/s) | Achieved Rate (k ops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) |")
        out.append("|---|---:|---:|---:|---:|---:|")
        for r in sweep_rows:
            tgt = "unpaced (max)" if float(r["target_rate_kops"]) == 0.0 else fmt(r["target_rate_kops"], 0)
            out.append(
                f"| `{r['variant']}` | {tgt} | {fmt(r['achieved_rate_kops'], 1)} | "
                f"{fmt(r['p50_ns'], 1)} | {fmt(r['p99_ns'], 1)} | {fmt(r['p999_ns'], 1)} |"
            )
        out.append("")

    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-dir", default="results")
    ap.add_argument("--out-md", default="results/comparison_report.md")
    ap.add_argument("--out-svg", default="")
    args = ap.parse_args()

    md = build_report(args.results_dir)
    os.makedirs(os.path.dirname(os.path.abspath(args.out_md)), exist_ok=True)
    with open(args.out_md, "w", encoding="utf-8") as f:
        f.write(md)

    if args.out_svg:
        sweep_rows = load_csv(os.path.join(args.results_dir, "matching_throughput_latency.csv"))
        generate_throughput_latency_svg(sweep_rows, args.out_svg)

    print(f"Wrote report to {args.out_md}")
    if args.out_svg:
        print(f"Wrote SVG plot to {args.out_svg}")


if __name__ == "__main__":
    main()
