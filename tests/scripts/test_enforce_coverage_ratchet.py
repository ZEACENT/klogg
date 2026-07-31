import importlib.util
import pathlib
import subprocess
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "enforce_coverage_ratchet.py"
SPEC = importlib.util.spec_from_file_location("enforce_coverage_ratchet", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


SUMMARY = "lines: 62.0%\nbranches: 33.0%\n"


class CoverageRatchetTest(unittest.TestCase):
    def test_rejects_non_finite_and_out_of_range_floors(self):
        for value in ("NaN", "inf", "-inf", "0", "101"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    MODULE.floor_value("floor", value)

    def test_rejects_floor_downgrade_even_when_measurement_is_high(self):
        issues = MODULE.evaluate(
            "lines: 100.0%\nbranches: 100.0%\n",
            "60.0",
            "32.6",
            base_line_floor_text="61.1",
            base_branch_floor_text="32.6",
        )
        self.assertIn("line coverage floor 60.0% is below the base floor 61.1%", issues)

    def test_allows_bootstrap_from_zero_and_monotonic_increase(self):
        self.assertEqual(
            MODULE.evaluate(
                SUMMARY,
                "61.1",
                "32.6",
                base_line_floor_text="0",
                base_branch_floor_text="0",
            ),
            [],
        )

    def test_measured_coverage_must_reach_candidate_floor(self):
        issues = MODULE.evaluate(
            SUMMARY,
            "63.0",
            "34.0",
            base_line_floor_text="61.1",
            base_branch_floor_text="32.6",
        )
        self.assertEqual(len(issues), 2)

    def test_missing_base_floor_bootstraps_from_zero(self):
        completed = lambda returncode, stderr="", stdout="": subprocess.CompletedProcess(
            args=[], returncode=returncode, stdout=stdout, stderr=stderr
        )
        with mock.patch.object(
            MODULE.subprocess,
            "run",
            side_effect=[completed(0), completed(128, stderr="path does not exist")],
        ):
            self.assertEqual(MODULE.git_file("base", pathlib.Path("floor.txt")), "0")

    def test_invalid_base_commit_still_fails_closed(self):
        completed = subprocess.CompletedProcess(
            args=[], returncode=128, stdout="", stderr="bad revision"
        )
        with mock.patch.object(MODULE.subprocess, "run", return_value=completed):
            with self.assertRaises(ValueError):
                MODULE.git_file("missing", pathlib.Path("floor.txt"))

    def test_missing_summary_percentage_fails_closed(self):
        with self.assertRaises(ValueError):
            MODULE.evaluate("lines: 62.0%\n", "61.1", "32.6")


if __name__ == "__main__":
    unittest.main()
