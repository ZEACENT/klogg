import io
import pathlib
import runpy
import subprocess
import sys
import unittest
from unittest import mock

from contract_cli_observation import ContractCliObserver


ROOT = pathlib.Path(__file__).parents[2]
HARDENING_DRIVER = ROOT / "tests/scripts/test_adb_helper_hardening_contract.py"


class SharedCliObservationContractTest(unittest.TestCase):
    def test_contract_counters_and_labels_are_independent(self):
        first = ContractCliObserver(enabled=True, label="first-cli")
        second = ContractCliObserver(enabled=True, label="second-cli")
        command = ["tar", "private-archive-argument"]
        completed = subprocess.CompletedProcess(command, 0)
        stream = io.StringIO()
        with mock.patch.object(sys, "stderr", stream), \
                mock.patch.object(subprocess, "run", return_value=completed) as invoke:
            for observer in (first, second, first):
                self.assertIs(observer(command, role="fixture-tar", check=True), completed)
        self.assertEqual(invoke.call_args_list, [mock.call(command, check=True)] * 3)
        self.assertIn("first-cli[1] before role=fixture-tar", stream.getvalue())
        self.assertIn("second-cli[1] before role=fixture-tar", stream.getvalue())
        self.assertIn("first-cli[2] returned returncode=0", stream.getvalue())
        self.assertNotIn("private-archive-argument", stream.getvalue())

    def test_quiet_observer_preserves_options_and_does_not_advance_counter(self):
        observer = ContractCliObserver(label="quiet-cli")
        command = ["tar", "fixture"]
        completed = subprocess.CompletedProcess(command, 17)
        stream = io.StringIO()
        with mock.patch.object(sys, "stderr", stream), \
                mock.patch.object(subprocess, "run", return_value=completed) as invoke:
            self.assertIs(observer(command, role="fixture-tar", check=False), completed)
        invoke.assert_called_once_with(command, check=False)
        self.assertEqual(observer.sequence, 0)
        self.assertEqual(stream.getvalue(), "")

    def test_broken_observation_stream_preserves_success_and_original_exception(self):
        observer = ContractCliObserver(enabled=True)
        command = ["model-python", "fixture.py"]
        stream = io.StringIO()
        completed = subprocess.CompletedProcess(command, 17)
        failure = OSError("private failure details")
        with mock.patch.object(sys, "stderr", stream), \
                mock.patch.object(stream, "write", side_effect=OSError("broken observer")):
            with mock.patch.object(subprocess, "run", return_value=completed):
                self.assertIs(observer(command, timeout=10), completed)
            with mock.patch.object(subprocess, "run", side_effect=failure):
                with self.assertRaises(OSError) as caught:
                    observer(command, timeout=10)
        self.assertIs(caught.exception, failure)

    def test_role_is_bounded_and_never_echoes_inline_body(self):
        observer = ContractCliObserver(enabled=True)
        command = ["model-python", "-c", "private-inline-body"]
        completed = subprocess.CompletedProcess(command, 0)
        stream = io.StringIO()
        with mock.patch.object(sys, "stderr", stream), \
                mock.patch.object(subprocess, "run", return_value=completed):
            observer(command)
            observer(command, role="x" * 4096)
        self.assertIn("before role=inline", stream.getvalue())
        self.assertIn("before role=" + "x" * 96 + "\n", stream.getvalue())
        self.assertNotIn("private-inline-body", stream.getvalue())
        self.assertLess(len(stream.getvalue()), 512)


class HardeningCliObservationContractTest(unittest.TestCase):
    class ObservedStream(io.StringIO):
        def __init__(self):
            super().__init__()
            self.flushes = 0

        def flush(self):
            self.flushes += 1
            super().flush()

    def load_main(self, stream):
        with mock.patch.object(sys, "stderr", stream), mock.patch.object(unittest, "main") as entry:
            namespace = runpy.run_path(str(HARDENING_DRIVER), run_name="__main__")
        return namespace, entry

    def test_direct_driver_announces_import_and_each_test_method(self):
        stream = self.ObservedStream()
        _, entry = self.load_main(stream)
        self.assertEqual(stream.getvalue(), "adb-hardening-contract: importing\n")
        self.assertGreater(stream.flushes, 0)
        entry.assert_called_once_with(verbosity=2)

    def test_direct_prefetch_cli_observation_precedes_original_invocation(self):
        stream = self.ObservedStream()
        namespace, _ = self.load_main(stream)
        case = namespace["AdbHelperSourceHardeningContractTest"]()
        result = subprocess.CompletedProcess(["original"], 17, "original-json", "private-stderr")

        def invoke(command, **kwargs):
            self.assertEqual(command[0], sys.executable)
            self.assertEqual(pathlib.Path(command[1]).name, "prefetch_adb_helper_sources.py")
            self.assertEqual(kwargs, {"capture_output": True, "text": True, "timeout": 10, "check": False})
            self.assertIn("adb-hardening-cli[1] before role=prefetch_adb_helper_sources.py", stream.getvalue())
            self.assertNotIn("returned", stream.getvalue())
            self.assertGreater(stream.flushes, 0)
            return result

        with mock.patch.object(sys, "stderr", stream), mock.patch.object(subprocess, "run", side_effect=invoke):
            actual = case.run_prefetch(pathlib.Path("fixture-lock"), pathlib.Path("fixture-downloads"))
        self.assertIs(actual, result)
        self.assertIn("adb-hardening-cli[1] returned returncode=17", stream.getvalue())
        self.assertNotIn("original-json", stream.getvalue())
        self.assertNotIn("private-stderr", stream.getvalue())

    def test_direct_legal_cli_timeout_keeps_original_exception_and_label(self):
        stream = self.ObservedStream()
        namespace, _ = self.load_main(stream)
        case = namespace["AdbHelperSourceHardeningContractTest"]()
        failure = subprocess.TimeoutExpired(["private-command"], 10)

        def invoke(command, **kwargs):
            self.assertEqual(pathlib.Path(command[1]).name, "build_adb_helper_legal_assets.py")
            self.assertEqual(kwargs["timeout"], 10)
            self.assertIn("adb-hardening-cli[1] before role=build_adb_helper_legal_assets.py", stream.getvalue())
            raise failure

        with mock.patch.object(sys, "stderr", stream), mock.patch.object(subprocess, "run", side_effect=invoke):
            with self.assertRaises(subprocess.TimeoutExpired) as caught:
                case.build_legal_assets(*(pathlib.Path(name) for name in ("repo", "archives", "lock", "output")))
        self.assertIs(caught.exception, failure)
        self.assertIn("adb-hardening-cli[1] timeout", stream.getvalue())
        self.assertNotIn("private-command", stream.getvalue())


if __name__ == "__main__":
    unittest.main()
