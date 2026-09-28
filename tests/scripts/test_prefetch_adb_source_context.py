"""The CI ADB source wrapper retains the failing locked URL without changing the image recipe."""

from __future__ import annotations

import contextlib
import io
import pathlib
import sys
import unittest
import urllib.error
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import prefetch_adb_source_context as context


class AdbSourceContextTest(unittest.TestCase):
    def test_success_delegates_to_the_unchanged_shared_prefetch(self):
        with mock.patch.object(context.prefetch, "main", return_value=0) as prefetch:
            self.assertEqual(context.main(), 0)
        prefetch.assert_called_once_with()

    def test_terminal_http_error_reports_the_exact_failing_url(self):
        url = "https://android.googlesource.com/platform/packages/modules/adb/+archive/revision.tar.gz"
        error = urllib.error.HTTPError(url, 503, "unavailable", {}, io.BytesIO())
        self.addCleanup(error.close)
        output = io.StringIO()
        with mock.patch.object(context.prefetch, "main", side_effect=error), \
                contextlib.redirect_stderr(output):
            with self.assertRaises(RuntimeError) as raised:
                context.main()
        self.assertIs(raised.exception.__cause__, error)
        self.assertIn(url, str(raised.exception))
        self.assertIn("503", str(raised.exception))
        self.assertIn(url, output.getvalue())


if __name__ == "__main__":
    unittest.main()
