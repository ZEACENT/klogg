"""Final-package CRT checks must not depend on runtimes installed on the host."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "verify_windows_runtime", ROOT / "scripts" / "verify_windows_runtime.py"
)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)

X64_RUNTIME = (
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
)


class WindowsRuntimeTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.staged = self.root / "release"
        self.package = self.root / "installed package"
        self.staged.mkdir()
        self.package.mkdir()
        for name in X64_RUNTIME:
            content = f"runtime fixture: {name}".encode("ascii")
            (self.staged / name).write_bytes(content)
            (self.package / name).write_bytes(content)

    def verify(self, arch="x64"):
        VERIFY.verify_runtime(self.package, self.staged, arch)

    def test_complete_x64_package_matches_staged_runtime(self):
        self.verify()

    def test_retired_x86_runtime_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsupported Windows runtime architecture"):
            self.verify("x86")

    def test_each_required_dll_must_be_in_final_package(self):
        for name in X64_RUNTIME:
            with self.subTest(runtime=name):
                path = self.package / name
                content = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, name):
                    self.verify()
                path.write_bytes(content)

    def test_missing_dll_cannot_be_satisfied_by_host_or_nested_copy(self):
        name = "msvcp140_2.dll"
        nested = self.package / "helpers"
        nested.mkdir()
        (self.package / name).rename(nested / name)
        with mock.patch.dict(os.environ, {"PATH": str(self.staged)}):
            with self.assertRaisesRegex(ValueError, name):
                self.verify()

    def test_dll_missing_from_both_stage_and_package_is_not_ignored(self):
        name = "msvcp140_2.dll"
        for root in (self.staged, self.package):
            (root / name).unlink()
        with self.assertRaisesRegex(ValueError, name):
            self.verify()

    def test_empty_or_directory_runtime_is_rejected(self):
        for root in (self.staged, self.package):
            for kind in ("empty", "directory"):
                with self.subTest(root=root.name, kind=kind):
                    path = root / "msvcp140_2.dll"
                    content = path.read_bytes()
                    path.unlink()
                    if kind == "empty":
                        path.touch()
                    else:
                        path.mkdir()
                    with self.assertRaisesRegex(ValueError, "msvcp140_2.dll"):
                        self.verify()
                    if kind == "directory":
                        path.rmdir()
                    path.write_bytes(content)

    def test_same_length_changed_dll_is_rejected(self):
        path = self.package / "msvcp140_2.dll"
        content = path.read_bytes()
        path.write_bytes(b"X" + content[1:])
        with self.assertRaisesRegex(ValueError, "msvcp140_2.dll.*differs"):
            self.verify()

    def test_missing_staging_reference_is_rejected(self):
        (self.staged / "msvcp140_2.dll").unlink()
        with self.assertRaisesRegex(ValueError, "msvcp140_2.dll"):
            self.verify()

    def test_staging_cannot_be_used_as_final_package(self):
        with self.assertRaisesRegex(ValueError, "distinct"):
            VERIFY.verify_runtime(self.staged / ".", self.staged, "x64")

    def test_unknown_architecture_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "architecture"):
            self.verify("arm64")

    def test_cli_reports_success_and_missing_dll_failure(self):
        arguments = ["--package-root", str(self.package),
                     "--staged-root", str(self.staged), "--arch", "x64"]
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(VERIFY.main(arguments), 0)
        self.assertIn(str(self.package), output.getvalue())
        (self.package / "msvcp140_2.dll").unlink()
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(VERIFY.main(arguments), 1)
        self.assertIn("msvcp140_2.dll", errors.getvalue())
        self.assertIn(str(self.package), errors.getvalue())


if __name__ == "__main__":
    unittest.main()
