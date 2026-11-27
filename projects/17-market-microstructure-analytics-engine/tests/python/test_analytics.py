"""End-to-end unit, integration, causal-invariance, reproducibility, and operational tests."""

from __future__ import annotations

import csv
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT_ROOT / "python"))

from mmae_analytics.evaluator import EvaluationConfig, evaluate_features
from mmae_analytics.fixtures import generate_synthetic_signed_flow_csv


def _find_cli() -> Path:
    env_bin = os.environ.get("MMAE_CLI_BIN")
    if env_bin:
        return Path(env_bin)
    candidate = PROJECT_ROOT / "build" / "mmae_cli"
    if candidate.exists():
        return candidate
    raise RuntimeError(f"mmae_cli binary not found at {candidate}; build project first.")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class TestMicrostructureEngine(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cli = _find_cli()
        cls.fixtures = PROJECT_ROOT / "fixtures"

    def test_hand_worked_fixture_matches_expected_exactly(self) -> None:
        expected = json.loads((self.fixtures / "hand_worked_l3_expected.json").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            for variant in ("baseline", "optimized"):
                feat_csv = t / f"features_{variant}.csv"
                diag_json = t / f"diag_{variant}.json"
                proc = subprocess.run(
                    [
                        str(self.cli),
                        "--input",
                        str(self.fixtures / "hand_worked_l3.csv"),
                        "--output-features",
                        str(feat_csv),
                        "--output-diagnostics",
                        str(diag_json),
                        "--variant",
                        variant,
                        "--depth-k",
                        "2",
                        "--tick-size",
                        "0.01",
                        "--strict-fail-on-invalid",
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(proc.returncode, 0, f"{variant} CLI failed: {proc.stderr}")

                with feat_csv.open("r", encoding="utf-8") as f:
                    actual_rows = list(csv.DictReader(f))
                self.assertEqual(len(actual_rows), len(expected["rows"]))

                for act, exp in zip(actual_rows, expected["rows"]):
                    for key, exp_val in exp.items():
                        raw = act[key]
                        if isinstance(exp_val, float):
                            self.assertAlmostEqual(
                                float(raw),
                                exp_val,
                                places=5,
                                msg=f"{variant} seq={exp['seq_num']} col={key}",
                            )
                        elif isinstance(exp_val, int):
                            self.assertEqual(
                                int(raw),
                                exp_val,
                                msg=f"{variant} seq={exp['seq_num']} col={key}",
                            )
                        else:
                            self.assertEqual(
                                raw,
                                exp_val,
                                msg=f"{variant} seq={exp['seq_num']} col={key}",
                            )

            # Baseline and optimized must match byte-for-byte.
            self.assertEqual(
                (t / "features_baseline.csv").read_bytes(),
                (t / "features_optimized.csv").read_bytes(),
            )

    def test_edge_cases_crossed_stale_and_all_gaps_flagged(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            feat_csv = t / "edge_features.csv"
            diag_json = t / "edge_diag.json"
            proc = subprocess.run(
                [
                    str(self.cli),
                    "--input",
                    str(self.fixtures / "edge_cases_crossed_stale_gaps.csv"),
                    "--output-features",
                    str(feat_csv),
                    "--output-diagnostics",
                    str(diag_json),
                    "--stale-threshold-ns",
                    "1000000000",
                    "--strict-fail-on-invalid",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            diag = json.loads(diag_json.read_text(encoding="utf-8"))
            self.assertEqual(diag["book_health"]["gap_count"], 2)
            self.assertEqual(len(diag["gaps"]), 2)
            self.assertEqual(diag["gaps"][0]["expected_seq"], 11)
            self.assertEqual(diag["gaps"][0]["observed_seq"], 15)
            self.assertTrue(diag["gaps"][0]["resolved_by_snapshot"])
            self.assertEqual(diag["gaps"][1]["expected_seq"], 21)
            self.assertEqual(diag["gaps"][1]["observed_seq"], 25)
            self.assertFalse(diag["gaps"][1]["resolved_by_snapshot"])
            self.assertGreaterEqual(diag["book_health"]["crossed_book_events"], 1)
            self.assertGreaterEqual(diag["book_health"]["locked_book_events"], 1)
            self.assertGreaterEqual(diag["book_health"]["stale_book_events"], 1)
            self.assertEqual(diag["trade_classification"]["ambiguous_gap"], 1)
            self.assertEqual(diag["trade_classification"]["ambiguous_stale"], 1)

    def test_synthetic_signed_flow_integration_and_oos_evaluation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            synth_csv = t / "synthetic.csv"
            meta = generate_synthetic_signed_flow_csv(synth_csv, seed=42, n_steps=1200, inject_gaps=True)
            self.assertEqual(meta["injected_gaps"], 2)

            feat_base = t / "feat_base.csv"
            diag_base = t / "diag_base.json"
            feat_opt = t / "feat_opt.csv"
            diag_opt = t / "diag_opt.json"

            for variant, fpath, dpath in (
                ("baseline", feat_base, diag_base),
                ("optimized", feat_opt, diag_opt),
            ):
                proc = subprocess.run(
                    [
                        str(self.cli),
                        "--config",
                        str(self.fixtures / "synthetic_signed_flow_config.json"),
                        "--variant",
                        variant,
                        "--input",
                        str(synth_csv),
                        "--output-features",
                        str(fpath),
                        "--output-diagnostics",
                        str(dpath),
                        "--strict-fail-on-invalid",
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(proc.returncode, 0, f"{variant} failed: {proc.stderr}")

            # Parity check
            self.assertEqual(feat_base.read_bytes(), feat_opt.read_bytes())

            diag = json.loads(diag_opt.read_text(encoding="utf-8"))
            self.assertEqual(diag["book_health"]["gap_count"], 2)
            self.assertGreater(diag["trade_classification"]["ambiguous_midpoint"], 0)
            self.assertEqual(diag["trade_classification"]["ambiguous_gap"], 2)

            ev = evaluate_features(
                feat_opt,
                diag_opt,
                EvaluationConfig(horizons_ms=[50, 100, 250, 500], train_fraction=0.70),
            )
            self.assertEqual(len(ev["replay_gaps"]), 2)
            self.assertGreater(ev["coverage"]["classified_trade_coverage"], 0.80)
            self.assertGreater(ev["coverage"]["ambiguous_trade_ratio"], 0.03)

            # Known synthetic signed flow must exhibit positive short-horizon price response & adverse selection.
            for h_key in ("50ms", "100ms", "250ms"):
                pr = ev["price_response_by_horizon"][h_key]
                ad = ev["adverse_selection_by_horizon"][h_key]
                oos = ev["oos_predictive_by_horizon"][h_key]

                self.assertGreater(pr["n_classified_trades_evaluated"], 100)
                self.assertGreater(pr["mean_response_ticks"], 0.05, f"expected positive price response at {h_key}")
                self.assertGreater(ad["mean_adverse_selection_ticks"], 0.05)
                self.assertLess(ad["max_decomposition_identity_residual"], 1e-9)
                self.assertGreater(oos["oos_pearson_r"], 0.05, f"expected positive OOS correlation at {h_key}")
                self.assertGreater(oos["oos_r2_vs_zero_baseline"], 0.0, f"expected OOS R2 > 0 at {h_key}")
                self.assertGreater(oos["n_embargo_excluded"], 0, "embargo window must exclude boundary rows")

    def test_no_future_quote_leakage_end_to_end(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            full_csv = t / "full.csv"
            generate_synthetic_signed_flow_csv(full_csv, seed=7, n_steps=60, inject_gaps=False)
            lines = full_csv.read_text(encoding="utf-8").splitlines()
            header = lines[0]
            data_lines = lines[1:]
            cut = 80

            # Build two streams sharing identical prefix [0:cut] but completely different futures after cut.
            stream_a = t / "stream_a.csv"
            stream_b = t / "stream_b.csv"
            stream_a.write_text("\n".join([header] + data_lines) + "\n", encoding="utf-8")

            last_pref = data_lines[cut - 1].split(",")
            ts_next = int(last_pref[0]) + 5_000_000
            seq_next = int(last_pref[2]) + 1
            sym = last_pref[3]
            mutated_tail = [
                f"{ts_next},{ts_next+100},{seq_next},{sym},L2_SET,BID,12000,99999,0",
                f"{ts_next+1_000_000},{ts_next+1_000_100},{seq_next+1},{sym},L2_SET,ASK,9000,99999,0",
                f"{ts_next+2_000_000},{ts_next+2_000_100},{seq_next+2},{sym},TRADE,UNKNOWN,11000,500,0",
            ]
            stream_b.write_text("\n".join([header] + data_lines[:cut] + mutated_tail) + "\n", encoding="utf-8")

            fa, fb = t / "fa.csv", t / "fb.csv"
            for inp, outf in ((stream_a, fa), (stream_b, fb)):
                proc = subprocess.run(
                    [
                        str(self.cli),
                        "--input",
                        str(inp),
                        "--output-features",
                        str(outf),
                        "--output-diagnostics",
                        str(t / (outf.stem + "_diag.json")),
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(proc.returncode, 0, proc.stderr)

            rows_a = fa.read_text(encoding="utf-8").splitlines()[: cut + 1]
            rows_b = fb.read_text(encoding="utf-8").splitlines()[: cut + 1]
            self.assertEqual(rows_a, rows_b, "Future quote mutation altered historical causal features!")

    def test_reproducibility_fixed_seed_identical_hashes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            hashes = []
            for run in (1, 2):
                inp = t / f"in_{run}.csv"
                feat = t / f"feat_{run}.csv"
                diag = t / f"diag_{run}.json"
                generate_synthetic_signed_flow_csv(inp, seed=99, n_steps=200, inject_gaps=True)
                proc = subprocess.run(
                    [
                        str(self.cli),
                        "--input",
                        str(inp),
                        "--output-features",
                        str(feat),
                        "--output-diagnostics",
                        str(diag),
                        "--run-id",
                        "repro-check",
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(proc.returncode, 0, proc.stderr)
                ev = evaluate_features(feat, diag)
                ev["features_csv"] = "normalized"
                hashes.append((_sha256(inp), _sha256(feat), _sha256(diag), json.dumps(ev, sort_keys=True)))

            self.assertEqual(hashes[0], hashes[1])

    def test_operational_failure_modes_no_silent_success(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            # 1. Missing config file
            p1 = subprocess.run(
                [
                    str(self.cli),
                    "--config",
                    str(t / "does_not_exist.json"),
                    "--input",
                    str(self.fixtures / "hand_worked_l3.csv"),
                    "--output-features",
                    str(t / "f.csv"),
                    "--output-diagnostics",
                    str(t / "d.json"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(p1.returncode, 0)
            self.assertIn("Config load failed", p1.stderr)

            # 2. Unsupported variant
            p2 = subprocess.run(
                [
                    str(self.cli),
                    "--variant",
                    "unsupported_book",
                    "--input",
                    str(self.fixtures / "hand_worked_l3.csv"),
                    "--output-features",
                    str(t / "f.csv"),
                    "--output-diagnostics",
                    str(t / "d.json"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(p2.returncode, 0)
            self.assertIn("Invalid configuration", p2.stderr)

            # 3. Missing input CSV
            p3 = subprocess.run(
                [
                    str(self.cli),
                    "--input",
                    str(t / "missing.csv"),
                    "--output-features",
                    str(t / "f.csv"),
                    "--output-diagnostics",
                    str(t / "d.json"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(p3.returncode, 0)
            self.assertIn("Failed to open input CSV", p3.stderr)

            # 4. Invalid rows under --strict-fail-on-invalid fail cleanly & write structured diagnostics
            diag_invalid = t / "invalid_diag.json"
            p4 = subprocess.run(
                [
                    str(self.cli),
                    "--input",
                    str(self.fixtures / "invalid_events.csv"),
                    "--output-features",
                    str(t / "invalid_feat.csv"),
                    "--output-diagnostics",
                    str(diag_invalid),
                    "--strict-fail-on-invalid",
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(p4.returncode, 5)
            diag = json.loads(diag_invalid.read_text(encoding="utf-8"))
            self.assertEqual(diag["counts"]["accepted_events"], 2)
            self.assertEqual(diag["counts"]["rejected_events"], 9)
            self.assertEqual(len(diag["validation_errors"]), 9)


if __name__ == "__main__":
    unittest.main()
