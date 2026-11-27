"""CLI entry point for fixture generation, evaluation, and baseline/optimized parity verification."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import List, Optional

from .evaluator import EvaluationConfig, evaluate_features
from .fixtures import generate_synthetic_signed_flow_csv


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="mmae-eval",
        description="Market Microstructure Analytics Engine — Fixture & Chronological Evaluation CLI",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_gen = sub.add_parser("generate-synthetic", help="Generate deterministic synthetic signed-flow CSV")
    p_gen.add_argument("--output", required=True, type=Path)
    p_gen.add_argument("--seed", type=int, default=42)
    p_gen.add_argument("--steps", type=int, default=1200)
    p_gen.add_argument("--symbol", type=str, default="SYNTH_FLOW")
    p_gen.add_argument("--no-gaps", action="store_true")

    p_eval = sub.add_parser("evaluate", help="Compute price response, adverse selection, and OOS metrics")
    p_eval.add_argument("--features", required=True, type=Path)
    p_eval.add_argument("--diagnostics", type=Path, default=None)
    p_eval.add_argument("--output", required=True, type=Path)
    p_eval.add_argument("--horizons-ms", type=str, default="50,100,250,500")
    p_eval.add_argument("--train-fraction", type=float, default=0.70)
    p_eval.add_argument("--tick-size", type=float, default=0.01)
    p_eval.add_argument("--stale-threshold-ns", type=int, default=1_000_000_000)

    p_par = sub.add_parser("verify-parity", help="Verify exact feature parity between baseline and optimized outputs")
    p_par.add_argument("--baseline-features", required=True, type=Path)
    p_par.add_argument("--optimized-features", required=True, type=Path)
    p_par.add_argument("--output", required=True, type=Path)

    args = parser.parse_args(argv)

    if args.command == "generate-synthetic":
        summary = generate_synthetic_signed_flow_csv(
            output_path=args.output,
            seed=args.seed,
            n_steps=args.steps,
            symbol=args.symbol,
            inject_gaps=not args.no_gaps,
        )
        print(json.dumps(summary, indent=2))
        return 0

    if args.command == "evaluate":
        if not args.features.exists():
            print(f"[ERROR] Features CSV not found: {args.features}", file=sys.stderr)
            return 2
        if args.diagnostics is not None and not args.diagnostics.exists():
            print(f"[ERROR] Diagnostics JSON not found: {args.diagnostics}", file=sys.stderr)
            return 2
        horizons = [int(x.strip()) for x in args.horizons_ms.split(",") if x.strip()]
        if not horizons or any(h <= 0 for h in horizons):
            print("[ERROR] --horizons-ms must contain positive integers", file=sys.stderr)
            return 2
        cfg = EvaluationConfig(
            horizons_ms=horizons,
            train_fraction=args.train_fraction,
            tick_size=args.tick_size,
            stale_threshold_ns=args.stale_threshold_ns,
        )
        report = evaluate_features(args.features, args.diagnostics, cfg)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report, indent=2))
        return 0

    if args.command == "verify-parity":
        if not args.baseline_features.exists() or not args.optimized_features.exists():
            print("[ERROR] Both baseline and optimized features CSV files must exist.", file=sys.stderr)
            return 2
        base_bytes = args.baseline_features.read_bytes()
        opt_bytes = args.optimized_features.read_bytes()
        exact_match = base_bytes == opt_bytes
        res = {
            "baseline_features": str(args.baseline_features),
            "optimized_features": str(args.optimized_features),
            "baseline_sha256": _sha256(args.baseline_features),
            "optimized_sha256": _sha256(args.optimized_features),
            "exact_byte_match": exact_match,
            "row_count": max(0, len(base_bytes.splitlines()) - 1),
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(res, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(res, indent=2))
        return 0 if exact_match else 1

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
