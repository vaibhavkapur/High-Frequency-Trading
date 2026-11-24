# Generated Results Directory

All files under `results/` are reproducible outputs generated directly by the C++ binaries and Python experiment scripts — no numbers are hand-edited or fabricated.

- `results/benchmarks/`: Raw CSV, stdout, and machine/compiler metadata produced by `scripts/run_benchmarks.sh`.
- `results/experiments/`: 12-session Train/Validation/Test summaries, per-session `summary.json`/`fills.csv`/`equity.csv`, robustness sweep CSVs, and independent `audit_report.json` produced by `scripts/run_experiments.py`.
- `results/figures/`: Matplotlib visualizations produced by `python/analytics.py`.
- `results/demo/`: Complete CLI demo output produced by `scripts/demo.sh`.
