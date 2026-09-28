from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import threading
import unittest
import urllib.error
from unittest import mock

ROOT = pathlib.Path(__file__).parents[2]
PREFETCH_SCRIPT = ROOT / "scripts" / "prefetch_adb_helper_sources.py"


def load_prefetch_module():
    spec = importlib.util.spec_from_file_location(
        "prefetch_adb_helper_sources", PREFETCH_SCRIPT
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()
        return False


def http_error(code: int) -> urllib.error.HTTPError:
    return urllib.error.HTTPError(
        "https://example.invalid/archive.tar.gz",
        code,
        "status",
        {},
        io.BytesIO(b""),
    )


class PrefetchDownloadRetryTest(unittest.TestCase):
    def responder(self, responses):
        def respond(*args, **kwargs):
            response = responses.pop(0)
            if isinstance(response, BaseException):
                raise response
            return response

        return respond
    def test_transient_server_errors_are_retried_until_success(self):
        module = load_prefetch_module()
        responses = [
            http_error(503),
            http_error(502),
            FakeResponse(b"archive-bytes"),
        ]
        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request,
            "urlopen",
            side_effect=self.responder(responses),
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            module.download(
                "https://example.invalid/archive.tar.gz",
                destination,
                sleep=sleeps.append,
            )
            self.assertEqual(b"archive-bytes", destination.read_bytes())
        self.assertEqual([1, 2], sleeps)

    def test_persistent_server_error_fails_after_bounded_attempts(self):
        module = load_prefetch_module()
        attempts: list[int] = []

        def fail(*args, **kwargs):
            attempts.append(1)
            raise http_error(503)

        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request, "urlopen", side_effect=fail
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            with self.assertRaises(urllib.error.HTTPError):
                module.download(
                    "https://example.invalid/archive.tar.gz",
                    destination,
                    sleep=sleeps.append,
                )
            self.assertEqual(module.DOWNLOAD_ATTEMPTS, len(attempts))
            self.assertFalse(destination.exists())
        self.assertEqual([1, 2, 3], sleeps)

    def test_client_error_is_not_retried(self):
        module = load_prefetch_module()
        attempts: list[int] = []

        def fail(*args, **kwargs):
            attempts.append(1)
            raise http_error(404)

        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request, "urlopen", side_effect=fail
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            with self.assertRaises(urllib.error.HTTPError):
                module.download(
                    "https://example.invalid/archive.tar.gz",
                    destination,
                    sleep=sleeps.append,
                )
        self.assertEqual(1, len(attempts))
        self.assertEqual([], sleeps)

    def test_streaming_download_observes_cancellation_between_chunks(self):
        module = load_prefetch_module()
        cancel_event = threading.Event()

        class CancellingResponse:
            def __init__(self):
                self.read_count = 0

            def __enter__(self):
                return self

            def __exit__(self, *exc_info):
                return False

            def read(self, size=-1):
                self.read_count += 1
                if self.read_count == 1:
                    cancel_event.set()
                    return b"partial"
                raise AssertionError("download read again after cancellation")

        response = CancellingResponse()
        with mock.patch.object(
            module.urllib.request, "urlopen", return_value=response
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                module.download(
                    "https://example.invalid/archive.tar.gz",
                    destination,
                    cancel_event=cancel_event,
                )
            self.assertEqual(response.read_count, 1)
            self.assertFalse(destination.exists())

    def test_successful_download_leaves_no_temporary_files(self):
        module = load_prefetch_module()
        with mock.patch.object(
            module.urllib.request,
            "urlopen",
            side_effect=lambda *a, **k: FakeResponse(b"archive-bytes"),
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            module.download(
                "https://example.invalid/archive.tar.gz",
                destination,
                sleep=self.fail,  # type: ignore[arg-type]
            )
            self.assertEqual(
                ["archive.tar.gz"],
                sorted(entry.name for entry in pathlib.Path(parent).iterdir()),
            )


class PrefetchConcurrencyTest(unittest.TestCase):
    MAX_DOWNLOAD_WORKERS = 4

    def locked_records(self, count: int) -> tuple[dict, dict[str, bytes]]:
        payloads = {
            f"source-{index}": f"archive-{index}".encode()
            for index in range(count)
        }
        lock = {
            "sources": [
                {
                    "id": source_id,
                    "archive_file": f"{source_id}.tar.gz",
                    "archive_url": f"https://example.invalid/{source_id}.tar.gz",
                    "archive_sha256": hashlib.sha256(payload).hexdigest(),
                }
                for source_id, payload in payloads.items()
            ]
        }
        return lock, payloads

    def run_prefetch_in_thread(
        self,
        module,
        lock_path: pathlib.Path,
        download_root: pathlib.Path,
    ) -> tuple[threading.Thread, list[BaseException]]:
        failures: list[BaseException] = []

        def invoke():
            try:
                with mock.patch.object(
                    sys,
                    "argv",
                    [
                        str(PREFETCH_SCRIPT),
                        "--lock",
                        str(lock_path),
                        "--download-root",
                        str(download_root),
                    ],
                ):
                    module.main()
            except BaseException as error:
                failures.append(error)

        worker = threading.Thread(target=invoke, daemon=True)
        worker.start()
        return worker, failures

    def test_prefetch_downloads_use_bounded_parallelism_without_time_sleeps(self):
        module = load_prefetch_module()
        lock, payloads = self.locked_records(8)
        release = threading.Event()
        capacity_active = threading.Event()
        too_many_active = threading.Event()
        state_lock = threading.Lock()
        active = 0
        maximum_active = 0

        def synchronized_download(url: str, destination: pathlib.Path, **kwargs):
            nonlocal active, maximum_active
            source_id = pathlib.PurePosixPath(url).name[: -len(".tar.gz")]
            with state_lock:
                active += 1
                maximum_active = max(maximum_active, active)
                if active >= self.MAX_DOWNLOAD_WORKERS:
                    capacity_active.set()
                if active > self.MAX_DOWNLOAD_WORKERS:
                    too_many_active.set()
            try:
                if not release.wait(timeout=20):
                    raise AssertionError("prefetch download release was not signaled")
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(payloads[source_id])
            finally:
                with state_lock:
                    active -= 1

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(module, "download", side_effect=synchronized_download):
                worker, failures = self.run_prefetch_in_thread(
                    module, lock_path, download_root
                )
                reached_capacity = capacity_active.wait(timeout=5)
                release.set()
                worker.join(timeout=5)
                overflowed = too_many_active.is_set()

            self.assertFalse(worker.is_alive(), "prefetch worker did not terminate")
            self.assertEqual(failures, [])
            self.assertTrue(
                reached_capacity,
                f"ADB prefetch did not start {self.MAX_DOWNLOAD_WORKERS} workers",
            )
            self.assertFalse(
                overflowed,
                f"ADB prefetch exceeded {self.MAX_DOWNLOAD_WORKERS} workers",
            )
            self.assertEqual(maximum_active, self.MAX_DOWNLOAD_WORKERS)

    def test_prefetch_manifest_order_is_lock_order_not_completion_order(self):
        module = load_prefetch_module()
        lock, payloads = self.locked_records(self.MAX_DOWNLOAD_WORKERS)
        release = {
            source_id: threading.Event() for source_id in payloads
        }
        completed = {
            source_id: threading.Event() for source_id in payloads
        }
        all_started = threading.Event()
        state_lock = threading.Lock()
        started: set[str] = set()

        def ordered_download(url: str, destination: pathlib.Path, **kwargs):
            source_id = pathlib.PurePosixPath(url).name[: -len(".tar.gz")]
            with state_lock:
                started.add(source_id)
                if len(started) == len(payloads):
                    all_started.set()
            if not release[source_id].wait(timeout=20):
                raise AssertionError(f"release not signaled for {source_id}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payloads[source_id])
            completed[source_id].set()

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(module, "download", side_effect=ordered_download):
                worker, failures = self.run_prefetch_in_thread(
                    module, lock_path, download_root
                )
                concurrent = all_started.wait(timeout=5)
                for source_id in reversed(list(payloads)):
                    release[source_id].set()
                    if concurrent:
                        self.assertTrue(
                            completed[source_id].wait(timeout=5),
                            f"download did not complete for {source_id}",
                        )
                worker.join(timeout=5)

            self.assertFalse(worker.is_alive(), "prefetch worker did not terminate")
            self.assertEqual(failures, [])
            self.assertTrue(
                concurrent,
                "deterministic ordering test requires all downloads to be in flight",
            )
            manifest = json.loads(
                (download_root / "adb-helper-prefetch-manifest.json").read_text()
            )
            self.assertEqual(
                [entry["id"] for entry in manifest["archives"]],
                list(payloads),
            )

    def test_prefetch_propagates_worker_failure_and_does_not_publish_manifest(self):
        module = load_prefetch_module()
        lock, payloads = self.locked_records(self.MAX_DOWNLOAD_WORKERS)

        def failing_download(url: str, destination: pathlib.Path, **kwargs):
            source_id = pathlib.PurePosixPath(url).name[: -len(".tar.gz")]
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payloads[source_id])
            if source_id == "source-1":
                raise RuntimeError("synthetic worker failure")

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(
                module, "download", side_effect=failing_download
            ), mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(download_root),
                ],
            ):
                with self.assertRaisesRegex(Exception, "synthetic worker failure"):
                    module.main()
            self.assertFalse(
                (download_root / "adb-helper-prefetch-manifest.json").exists()
            )

    def test_later_worker_failure_cancels_an_earlier_retry_without_submission_order_delay(self):
        module = load_prefetch_module()
        lock, _ = self.locked_records(2)
        first_started = threading.Event()
        cleanup_release = threading.Event()

        def cancellation_aware_download(
            url: str,
            destination: pathlib.Path,
            *,
            cancel_event=None,
            **kwargs,
        ):
            source_id = pathlib.PurePosixPath(url).name[: -len(".tar.gz")]
            if source_id == "source-0":
                first_started.set()
                if cancel_event is None:
                    cleanup_release.wait(timeout=20)
                else:
                    cancel_event.wait(timeout=20)
                return
            self.assertTrue(first_started.wait(timeout=5))
            raise RuntimeError("synthetic later worker failure")

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(
                module, "download", side_effect=cancellation_aware_download
            ):
                worker, failures = self.run_prefetch_in_thread(
                    module, lock_path, download_root
                )
                worker.join(timeout=2)
                completed_after_failure = not worker.is_alive()
                cleanup_release.set()
                worker.join(timeout=5)

            self.assertTrue(
                completed_after_failure,
                "a later worker failure was hidden behind an earlier submitted download",
            )
            self.assertEqual(len(failures), 1)
            self.assertIn("synthetic later worker failure", str(failures[0]))
            self.assertFalse(
                (download_root / "adb-helper-prefetch-manifest.json").exists()
            )

    def test_falsy_non_list_symlink_exclusions_fail_before_network(self):
        module = load_prefetch_module()
        lock, _ = self.locked_records(1)
        lock["sources"][0]["excluded_build_symlinks"] = False

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(module, "download") as download, mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(root / "downloads"),
                ],
            ):
                with self.assertRaisesRegex(
                    RuntimeError, "excluded build symlinks must be an array"
                ):
                    module.main()
            download.assert_not_called()

    def test_entire_lock_is_validated_before_any_download_starts(self):
        module = load_prefetch_module()
        lock, _ = self.locked_records(2)
        lock["sources"][1]["archive_sha256"] = "not-a-sha256"

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(module, "download") as download, mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(root / "downloads"),
                ],
            ):
                with self.assertRaisesRegex(RuntimeError, "invalid archive_sha256"):
                    module.main()
            download.assert_not_called()

    def test_offline_mode_avoids_executor_and_network(self):
        module = load_prefetch_module()
        lock, _ = self.locked_records(1)

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            with mock.patch.object(
                module.concurrent.futures, "ThreadPoolExecutor"
            ) as executor, mock.patch.object(module, "download") as download, mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(root / "downloads"),
                    "--offline",
                ],
            ):
                with self.assertRaisesRegex(RuntimeError, "archive is missing"):
                    module.main()
            executor.assert_not_called()
            download.assert_not_called()

    def test_offline_cache_rejects_unlocked_extra_entries(self):
        module = load_prefetch_module()
        lock, payloads = self.locked_records(1)

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            download_root.mkdir()
            (download_root / "source-0.tar.gz").write_bytes(payloads["source-0"])
            (download_root / "removed-source.tar.gz").write_bytes(b"stale")
            with mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(download_root),
                    "--offline",
                ],
            ):
                with self.assertRaisesRegex(RuntimeError, "unlocked entry"):
                    module.main()

    def test_worker_override_downloads_only_missing_archives(self):
        module = load_prefetch_module()
        lock, payloads = self.locked_records(3)

        def write_download(url: str, destination: pathlib.Path, **kwargs):
            source_id = pathlib.PurePosixPath(url).name[: -len(".tar.gz")]
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payloads[source_id])

        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            download_root = root / "downloads"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            download_root.mkdir()
            (download_root / "source-0.tar.gz").write_bytes(payloads["source-0"])
            real_executor = module.concurrent.futures.ThreadPoolExecutor
            with mock.patch.object(
                module.concurrent.futures,
                "ThreadPoolExecutor",
                wraps=real_executor,
            ) as executor, mock.patch.object(
                module, "download", side_effect=write_download
            ) as download, mock.patch.object(
                sys,
                "argv",
                [
                    str(PREFETCH_SCRIPT),
                    "--lock",
                    str(lock_path),
                    "--download-root",
                    str(download_root),
                    "--workers",
                    "2",
                ],
            ):
                module.main()

            executor.assert_called_once_with(max_workers=2)
            self.assertEqual(2, download.call_count)
            self.assertEqual(
                {"source-1.tar.gz", "source-2.tar.gz"},
                {call.args[1].name for call in download.call_args_list},
            )


if __name__ == "__main__":
    unittest.main()
