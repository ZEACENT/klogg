from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import sys
import unittest
from contextlib import redirect_stdout
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "run_ci_quality.py"
SPEC = importlib.util.spec_from_file_location("run_ci_quality", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class Result:
    def __init__(self, returncode: int = 0, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class RunCiQualityTest(unittest.TestCase):
    def test_commands_cover_every_fast_ci_quality_gate_in_order(self) -> None:
        commands = MODULE.quality_commands(ROOT)
        self.assertEqual(
            commands,
            [
                [sys.executable, "scripts/lint_platform_fragile.py"],
                [sys.executable, "scripts/lint_linux_package_runtime.py"],
                [sys.executable, "scripts/lint_ci_quality.py"],
                [sys.executable, "scripts/lint_translation_catalogs.py"],
                [
                    sys.executable,
                    "-m",
                    "unittest",
                    "discover",
                    "-s",
                    "tests/scripts",
                    "-p",
                    "test_*.py",
                ],
            ],
        )

    def test_failure_is_fail_fast_and_propagated(self) -> None:
        calls: list[list[str]] = []

        def run(command: list[str], **_: object) -> Result:
            calls.append(command)
            return Result(returncode=7 if len(calls) == 2 else 0)

        with mock.patch.object(MODULE.subprocess, "run", side_effect=run):
            result = MODULE.run_quality_checks(ROOT)

        self.assertEqual(result["returncode"], 7)
        self.assertEqual(len(result["checks"]), 2)
        self.assertEqual(calls, MODULE.quality_commands(ROOT)[:2])

    def test_json_output_contains_repository_identity_and_durations(self) -> None:
        completed = Result()
        with mock.patch.object(
            MODULE, "repository_identity", return_value=("a" * 40, True)
        ), mock.patch.object(
            MODULE.subprocess, "run", return_value=completed
        ), mock.patch.object(
            MODULE.time, "monotonic", side_effect=[1.0, 1.25] * 5
        ), mock.patch.object(sys, "argv", [str(SCRIPT), "--json"]):
            output = io.StringIO()
            with redirect_stdout(output):
                returncode = MODULE.main()

        payload = json.loads(output.getvalue())
        self.assertEqual(returncode, 0)
        self.assertEqual(payload["commit"], "a" * 40)
        self.assertIs(payload["dirty"], True)
        self.assertEqual(len(payload["checks"]), 5)
        self.assertTrue(all(check["durationSeconds"] == 0.25 for check in payload["checks"]))


if __name__ == "__main__":
    unittest.main()
