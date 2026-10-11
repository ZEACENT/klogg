"""Shared Actions transport keeps byte/deadline bounds and child cleanup."""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_actions_transport as transport
import ci_environment_source_cache as source_cache


class ActionsMetadataTest(unittest.TestCase):
    def test_metadata_uses_bounded_authenticated_request(self):
        result = subprocess.CompletedProcess([], 0, stdout=b'{"id":42}')
        with mock.patch.object(transport.subprocess, "run", return_value=result) as run:
            self.assertEqual(transport.metadata("repos/ZEACENT/klogg/actions/runs/42"), {"id": 42})
        run.assert_called_once_with(
            ["gh", "api", "repos/ZEACENT/klogg/actions/runs/42"],
            check=True, capture_output=True, timeout=30,
        )

    def test_malformed_duplicate_nonfinite_and_nonobject_metadata_fail(self):
        for raw in (b'not json', b'{"id":1,"id":2}', b'{"id":NaN}', b'[]'):
            with self.subTest(raw=raw):
                result = subprocess.CompletedProcess([], 0, stdout=raw)
                with mock.patch.object(transport.subprocess, "run", return_value=result):
                    with self.assertRaises(transport.TransportError):
                        transport.metadata("metadata")

    def test_post_capture_metadata_ceiling_is_preserved(self):
        result = subprocess.CompletedProcess(
            [], 0, stdout=b" " * (transport.core.MAX_METADATA_BYTES + 1),
        )
        with mock.patch.object(transport.subprocess, "run", return_value=result):
            with self.assertRaisesRegex(transport.TransportError, "oversized"):
                transport.metadata("metadata")

    def test_request_failure_preserves_source_cache_error_contract(self):
        for error in (OSError("missing gh"), subprocess.TimeoutExpired("gh", 30)):
            with self.subTest(error=type(error).__name__):
                with mock.patch.object(transport.subprocess, "run", side_effect=error):
                    with self.assertRaises(transport.TransportError):
                        transport.metadata("metadata")
                    with self.assertRaises(source_cache.SourceCacheError):
                        source_cache._metadata("metadata")


@unittest.skipIf(os.name == "nt", "Raw artifact acquisition is a POSIX producer operation")
class ActionsArchiveStreamTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.archive = pathlib.Path(temporary.name) / "archive.zip"
        self.processes = []
        self.commands = []
        self.real_popen = subprocess.Popen
        token = mock.patch.dict(transport.os.environ, {"GH_TOKEN": "fixture-read-token"})
        token.start()
        self.addCleanup(token.stop)

    def producer(self, script):
        def start(command, **kwargs):
            self.commands.append(command)
            process = self.real_popen([sys.executable, "-c", script], **kwargs)
            self.processes.append(process)
            return process
        return start

    def assert_reaped(self):
        self.assertEqual(len(self.processes), 1)
        self.assertIsNotNone(self.processes[0].poll())
        self.assertTrue(self.processes[0].stdout.closed)

    def test_exact_stream_uses_fixed_repository_and_returns_bounded_bytes(self):
        with mock.patch.object(transport.subprocess, "Popen", side_effect=self.producer(
                "import sys; sys.stdout.buffer.write(b'PK\\x00\\x1b\\xff')")):
            transport.download_artifact_zip(42, self.archive, limit=5, timeout=30)
        self.assertEqual(self.archive.read_bytes(), b"PK\x00\x1b\xff")
        self.assertEqual(self.commands[0], [
            "gh", "api", "--allow-escape-sequences",
            "repos/ZEACENT/klogg/actions/artifacts/42/zip",
        ])
        self.assert_reaped()

    def test_overflow_empty_and_nonzero_exit_fail_and_reap(self):
        cases = (("import sys; sys.stdout.buffer.write(b'x' * 100)", "byte limit"),
                 ("pass", "failed"),
                 ("import sys; sys.stdout.buffer.write(b'x'); sys.exit(7)", "failed"))
        for index, (script, message) in enumerate(cases):
            with self.subTest(index=index):
                archive = self.archive.with_name("case-{}.zip".format(index))
                self.processes.clear()
                with mock.patch.object(transport.subprocess, "Popen", side_effect=self.producer(script)):
                    with self.assertRaisesRegex(transport.TransportError, message):
                        transport.download_artifact_zip(42, archive, limit=10, timeout=30)
                self.assert_reaped()

    def test_expired_deadline_kills_and_reaps_live_child_without_polling_sleep(self):
        def start(command, **kwargs):
            kwargs["stdin"] = subprocess.PIPE
            return self.producer("import sys; sys.stdin.buffer.read()")(command, **kwargs)
        with mock.patch.object(transport.subprocess, "Popen", side_effect=start), \
                mock.patch.object(transport.time, "monotonic", side_effect=(0, 2)):
            with self.assertRaisesRegex(transport.TransportError, "deadline"):
                transport.download_artifact_zip(42, self.archive, limit=10, timeout=1)
        self.assert_reaped()
        self.processes[0].stdin.close()

    def test_existing_destination_and_missing_token_never_spawn(self):
        self.archive.write_bytes(b"owned bytes")
        with mock.patch.object(transport.subprocess, "Popen") as spawn:
            with self.assertRaises(FileExistsError):
                transport.download_artifact_zip(42, self.archive, limit=100, timeout=30)
            with mock.patch.dict(transport.os.environ, {"GH_TOKEN": ""}):
                with self.assertRaisesRegex(transport.TransportError, "token"):
                    transport.download_artifact_zip(42, self.archive, limit=100, timeout=30)
        spawn.assert_not_called()
        self.assertEqual(self.archive.read_bytes(), b"owned bytes")


if __name__ == "__main__":
    unittest.main()
