from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "run_perf_gates.py"
SPEC = importlib.util.spec_from_file_location("run_perf_gates", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class Result:
    def __init__(self, returncode: int = 0):
        self.returncode = returncode


class RunPerfGatesTest(unittest.TestCase):
    def test_ctest_runs_with_perf_gates_env_and_build_dir(self) -> None:
        calls = []

        def run(command, **kwargs):
            calls.append((command, kwargs))
            return Result()

        with tempfile.TemporaryDirectory() as build_dir:
            with mock.patch.object(MODULE.subprocess, "run", side_effect=run):
                returncode = MODULE.run_perf_gates(pathlib.Path(build_dir))

            self.assertEqual(returncode, 0)
            self.assertEqual(len(calls), 1)
            command, kwargs = calls[0]
            self.assertEqual(command[:1], ["ctest"])
            self.assertIn("--output-on-failure", command)
            self.assertEqual(kwargs["cwd"], pathlib.Path(build_dir))
            self.assertEqual(kwargs["env"]["KLOGG_PERF_GATES"], "1")

    def test_ctest_failure_is_propagated(self) -> None:
        with tempfile.TemporaryDirectory() as build_dir:
            with mock.patch.object(
                MODULE.subprocess, "run", return_value=Result(returncode=8)
            ):
                returncode = MODULE.run_perf_gates(pathlib.Path(build_dir))

        self.assertEqual(returncode, 8)

    def test_missing_build_dir_fails_closed(self) -> None:
        returncode = MODULE.run_perf_gates(pathlib.Path("/definitely/missing"))
        self.assertEqual(returncode, 2)


if __name__ == "__main__":
    unittest.main()
