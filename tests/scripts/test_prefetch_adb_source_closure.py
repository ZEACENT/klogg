"""One bounded ADB source path must serve ordinary CI and environment fixture."""

from __future__ import annotations

import contextlib
import io
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import prefetch_adb_source_closure as closure


class SourceClosureTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.lock = self.root / "lock.json"
        self.lock.write_text("{}", encoding="utf-8")
        self.downloads = self.root / "cache"
        self.calls = []

    def manifest(self, lock, root):
        self.calls.append(("manifest", lock, root))

    def libusb(self, lock, root):
        self.calls.append(("libusb", lock, root))

    def runner(self, command, **kwargs):
        self.calls.append(("complete", command, kwargs))
        return subprocess.CompletedProcess(command, 0, "locked ADB source closure\n", "")

    def invoke(self, **kwargs):
        return closure.prefetch_closure(
            self.lock, self.downloads,
            manifest=self.manifest, libusb=self.libusb,
            runner=self.runner, **kwargs,
        )

    def test_source_fallbacks_seed_cache_before_one_original_full_validator(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertIsNone(self.invoke())
        self.assertEqual([call[0] for call in self.calls],
                         ["manifest", "libusb", "complete"])
        command = self.calls[-1][1]
        self.assertEqual(command[:2], [sys.executable,
            str(ROOT / "scripts" / "prefetch_adb_source_context.py")])
        self.assertIn("--lock", command)
        self.assertEqual(command[command.index("--download-root") + 1], str(self.downloads))
        self.assertEqual(command[command.index("--workers") + 1], "2")
        self.assertEqual(command[command.index("--max-cache-bytes") + 1], "536870912")
        self.assertNotIn("--offline", command)
        self.assertIn("locked ADB source closure", output.getvalue())

    def test_503_context_is_last_exception_line_in_captured_producer(self):
        url = "https://android.googlesource.com/platform/external/libusb/+archive/locked.tar.gz"
        def unavailable(command, **kwargs):
            return subprocess.CompletedProcess(
                command, 1, "", "Traceback\nRuntimeError: ADB source fetch failed (" + url + "): HTTP 503\n"
            )
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaisesRegex(RuntimeError, re.escape(url)):
            closure.prefetch_closure(self.lock, self.downloads, manifest=self.manifest,
                                     libusb=self.libusb, runner=unavailable)
        self.assertIn(url, stderr.getvalue())
        self.assertEqual([call[0] for call in self.calls], ["manifest", "libusb"])

    def test_manifest_failure_prevents_other_downloads_and_full_validator(self):
        libusb = mock.Mock()
        runner = mock.Mock()
        with self.assertRaisesRegex(RuntimeError, "locked manifest"):
            closure.prefetch_closure(
                self.lock, self.downloads,
                manifest=mock.Mock(side_effect=RuntimeError("locked manifest differs")),
                libusb=libusb, runner=runner,
            )
        libusb.assert_not_called()
        runner.assert_not_called()

    def test_unreviewed_fanout_or_cache_budget_fails_before_network(self):
        for options in ({"workers": 4}, {"max_cache_bytes": 536870913}):
            with self.subTest(options=options), self.assertRaisesRegex(RuntimeError, "unreviewed"):
                self.invoke(**options)
            self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
