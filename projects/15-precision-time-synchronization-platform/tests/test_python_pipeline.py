#!/usr/bin/env python3
"""Python end-to-end verification tests for ptsp (stdlib unittest, no external packages required)."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
CLI = BUILD_DIR / "ptsp_cli"
FIXTURES = ROOT / "fixtures"


class TestPtspEndToEnd(unittest.TestCase):
    def test_reference_matches_independent_python_expectation(self) -> None:
        expected = json.loads((FIXTURES / "expected_reference.json").read_text(encoding="utf-8"))
        proc = subprocess.run(
            [str(CLI), "replay", "--config", str(FIXTURES / "reference_drift.cfg")],
            capture_output=True,
            text=True,
            check=True,
        )
        got = json.loads(proc.stdout)
        self.assertEqual(got["schema_version"], expected["schema_version"])
        self.assertEqual(got["sync_sample_count"], expected["sync_sample_count"])
        self.assertEqual(got["packet_count"], expected["packet_count"])
        self.assertEqual(got["orderer"]["final_state"], expected["expected_orderer_state"])
        self.assertEqual(got["orderer"]["packets_emitted"], expected["expected_packets_emitted"])
        self.assertEqual(got["error_source"], "SYNTHETIC_FIXTURE")
        self.assertLessEqual(got["robust_estimator"]["steady_state_max_error_ns"], 100)
        self.assertLessEqual(got["robust_estimator"]["mae_drift_ppb"], 15.0)

    def test_reproducibility_identical_fingerprints_across_reruns(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out1 = Path(tmp) / "r1.json"
            out2 = Path(tmp) / "r2.json"
            subprocess.run(
                [str(CLI), "replay", "--config", str(FIXTURES / "clock_step_and_outliers.cfg"), "--out", str(out1)],
                capture_output=True,
                text=True,
                check=True,
            )
            subprocess.run(
                [str(CLI), "replay", "--config", str(FIXTURES / "clock_step_and_outliers.cfg"), "--out", str(out2)],
                capture_output=True,
                text=True,
                check=True,
            )
            self.assertEqual(out1.read_text(encoding="utf-8"), out2.read_text(encoding="utf-8"))

    def test_probe_separates_measured_and_reports_hw_unavailable(self) -> None:
        proc = subprocess.run(
            [str(CLI), "probe", "--interface", "nonexistent0", "--ptp-device", "/dev/ptp_missing"],
            capture_output=True,
            text=True,
            check=True,
        )
        data = json.loads(proc.stdout)
        self.assertEqual(data["software_adapter"]["error_source"], "MEASURED")
        self.assertTrue(data["software_adapter"]["available"])
        self.assertFalse(data["software_adapter"]["hardware_timestamping_supported"])
        self.assertFalse(data["linux_ptp_hw_adapter"]["available"])
        self.assertEqual(data["linux_ptp_hw_adapter"]["status_code"], "HARDWARE_TIMESTAMP_UNAVAILABLE")

    def test_missing_config_fails_closed_nonzero(self) -> None:
        proc = subprocess.run(
            [str(CLI), "replay", "--config", str(FIXTURES / "no_such_config.cfg")],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("MISSING_CONFIG", proc.stderr)


if __name__ == "__main__":
    unittest.main()
