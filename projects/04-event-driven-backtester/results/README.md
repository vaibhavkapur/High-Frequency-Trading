# results/

Everything here is produced by a command in this repository; nothing is typed in by hand.

| Path | Produced by | Kept in git |
|---|---|---|
| `benchmarks/` | `scripts/run_benchmarks.sh` | yes: raw CSVs + `environment.txt` |
| `experiments/` | commands listed in `docs/experiments.md` | yes |
| anything else (`sample_*`, `tiny_demo`, ...) | `hftbt run` with `run.out_dir` | no (regenerate) |

All inputs are **synthetic** order flow from `hftbt gen`. The numbers measure this simulator and
this machine; they say nothing about real markets.
