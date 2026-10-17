import hashlib
import io
import json
import pathlib
import stat
import sys
import tarfile
import tempfile
import unittest
import urllib.error
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import materialize_windows_tools as tools
import prefetch_adb_helper_sources as transport


class WindowsToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.cache = self.root / "cache"
        self.destination = self.root / "tools.zip"
        self.lock = self.root / "lock.json"
        self.package = "mingw-w64-x86_64-ragel"
        self.version = "6.10-3"
        self.filename = f"{self.package}-{self.version}-any.pkg.tar.zst"
        self.members = {
            ".PKGINFO": f"pkgname = {self.package}\npkgver = {self.version}\n".encode(),
            "mingw64/bin/ragel.exe": b"ragel binary",
            "mingw64/share/licenses/ragel/COPYING": b"GPL",
        }
        self._make_package()

    def _make_package(self):
        contents = io.BytesIO()
        with tarfile.open(fileobj=contents, mode="w") as tar:
            for name, data in self.members.items():
                entry = tarfile.TarInfo(name)
                if name.endswith("/"):
                    entry.type = tarfile.DIRTYPE
                    tar.addfile(entry)
                else:
                    entry.size = len(data)
                    tar.addfile(entry, io.BytesIO(data))
        self.payload = contents.getvalue()
        self.data = b"fake compressed data"
        self.document = {"schema_version": 1, "packages": [{
            "name": "ragel", "version": self.version, "archive_file": self.filename,
            "archive_url": f"https://repo.msys2.org/mingw/mingw64/{self.filename}",
            "archive_sha256": hashlib.sha256(self.data).hexdigest(),
            "depends": [], "license": "GPL-2.0-or-later",
            "runtime_files": ["mingw64/bin/ragel.exe"],
        }]}
        self.lock.write_text(json.dumps(self.document))

    def _materialize(self, **kwargs):
        return tools.materialize(self.cache, self.destination, self.lock,
                                 decompressor=lambda archive: io.BytesIO(self.payload), **kwargs)

    def test_reviewed_caps_distinguish_compressed_archives_and_unpacked_members(self):
        self.assertEqual(tools.MAX_PACKAGE_SIZE, 512 * 1024 * 1024)
        self.assertEqual(tools.MAX_SOURCE_SIZE, 512 * 1024 * 1024)
        self.assertEqual(tools.MAX_ENTRY_SIZE, 128 * 1024 * 1024)

    def test_default_transport_accepts_exact_runtime_and_source_bounds(self):
        source = b"synthetic corresponding source"
        source_name = "mingw-w64-ragel-6.10-3.src.tar.zst"
        self.document["corresponding_sources"] = [{
            "name": "ragel", "archive_file": source_name,
            "archive_url": "https://repo.msys2.org/mingw/sources/" + source_name,
            "archive_sha256": hashlib.sha256(source).hexdigest(),
            "license_obligation": "synthetic source fixture",
        }]
        self.lock.write_text(json.dumps(self.document))
        responses = [io.BytesIO(self.data), io.BytesIO(source)]
        with mock.patch.object(tools, "MAX_PACKAGE_SIZE", len(self.data)), mock.patch.object(
            tools, "MAX_SOURCE_SIZE", len(source)
        ), mock.patch.object(transport.urllib.request, "urlopen", side_effect=responses) as opened, mock.patch.object(
            tools, "download_source", wraps=tools.download_source
        ) as download:
            self._materialize()
        self.assertEqual(download.call_count, 2)
        self.assertEqual([call.kwargs for call in download.call_args_list], [
            {"attempts": 1, "max_bytes": len(self.data), "socket_timeout": 90},
            {"attempts": 1, "max_bytes": len(source), "socket_timeout": 90},
        ])
        self.assertTrue(all(call.kwargs["timeout"] == 90 for call in opened.call_args_list))
        self.assertTrue(all(response.closed for response in responses))
        with zipfile.ZipFile(self.destination) as archive:
            self.assertEqual(archive.read("corresponding-source/" + source_name), source)

    def test_default_transport_preserves_three_outer_attempts_without_nested_retries(self):
        bodies = [io.BytesIO(), io.BytesIO()]
        failures = [urllib.error.HTTPError("https://repo.msys2.org/", code, "transient", {}, body)
                    for code, body in zip((503, 504), bodies)]
        with mock.patch.object(transport.urllib.request, "urlopen", side_effect=failures + [io.BytesIO(self.data)]) as opened, mock.patch.object(
            tools, "download_source", wraps=tools.download_source
        ) as download, mock.patch.object(tools.time, "sleep") as sleep:
            self._materialize()
        self.assertEqual(opened.call_count, 3)
        self.assertEqual(download.call_count, 3)
        self.assertEqual(sleep.call_args_list, [mock.call(1), mock.call(2)])
        self.assertTrue(all(call.kwargs == {"attempts": 1, "max_bytes": tools.MAX_PACKAGE_SIZE, "socket_timeout": 90}
                            for call in download.call_args_list))
        self.assertTrue(all(call.kwargs["timeout"] == 90 for call in opened.call_args_list))
        self.assertEqual((self.cache / self.filename).read_bytes(), self.data)
        self.assertTrue(all(body.closed for body in bodies))

    def test_default_transport_preserves_nonretryable_http_statuses(self):
        for code in (404, 501):
            with self.subTest(status=code):
                body = io.BytesIO()
                failure = urllib.error.HTTPError("https://repo.msys2.org/", code, "permanent", {}, body)
                with mock.patch.object(transport.urllib.request, "urlopen", side_effect=failure) as opened, mock.patch.object(
                    tools.time, "sleep", side_effect=AssertionError("permanent failure retried")
                ):
                    with self.assertRaisesRegex(RuntimeError, "rejected"):
                        self._materialize()
                opened.assert_called_once()
                self.assertTrue(body.closed)
                self.assertEqual(list(self.cache.iterdir()), [])
                self.assertFalse(self.destination.exists())

    def test_default_transport_bounds_runtime_and_corresponding_source_streams(self):
        class Response:
            def __init__(self):
                self.read_sizes = []
                self.closed = False

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                self.closed = True

            def read(self, size):
                self.read_sizes.append(size)
                if len(self.read_sizes) == 1:
                    return b"12345678"
                if len(self.read_sizes) == 2:
                    return b"9"
                raise AssertionError("MSYS2 read after the first overflow byte")

            read1 = read

        for source in (False, True):
            with self.subTest(corresponding_source=source):
                response = Response()
                document = json.loads(json.dumps(self.document))
                source_name = "mingw-w64-ragel-6.10-3.src.tar.zst"
                if source:
                    document["corresponding_sources"] = [{
                        "name": "ragel", "archive_file": source_name,
                        "archive_url": "https://repo.msys2.org/mingw/sources/" + source_name,
                        "archive_sha256": hashlib.sha256(b"123456789").hexdigest(),
                        "license_obligation": "synthetic source fixture",
                    }]
                self.lock.write_text(json.dumps(document))
                cache = self.root / ("source-cache" if source else "runtime-cache")
                def open_response(request, **kwargs):
                    address = getattr(request, "full_url", request)
                    if source and address.endswith(self.filename):
                        return io.BytesIO(self.data)
                    return response
                with mock.patch.object(tools, "MAX_PACKAGE_SIZE", 1024 if source else 8), mock.patch.object(
                    tools, "MAX_SOURCE_SIZE", 8
                ), mock.patch.object(transport.urllib.request, "urlopen", side_effect=open_response) as opened, mock.patch.object(
                    tools.time, "sleep", side_effect=AssertionError("oversize was retried")
                ):
                    with self.assertRaises(transport.DownloadSizeError):
                        tools.materialize(cache, self.destination, self.lock,
                                          decompressor=lambda archive: io.BytesIO(self.payload))
                self.assertEqual(opened.call_count, 2 if source else 1)
                self.assertTrue(all(call.kwargs["timeout"] == 90 for call in opened.call_args_list))
                self.assertEqual(response.read_sizes, [9, 1])
                self.assertTrue(response.closed)
                self.assertFalse(self.destination.exists())
                self.assertEqual({path.name for path in cache.iterdir()}, {self.filename} if source else set())

    def test_cached_and_injected_oversize_fail_before_hash_or_cache_publication(self):
        real_sha256 = hashlib.sha256
        for source in (False, True):
            for cached in (False, True):
                with self.subTest(corresponding_source=source, cached=cached):
                    cache = self.root / ("bounded-{}-{}".format(source, cached))
                    cache.mkdir()
                    document = json.loads(json.dumps(self.document))
                    name = self.filename
                    if source:
                        (cache / self.filename).write_bytes(self.data)
                        name = "mingw-w64-ragel-6.10-3.src.tar.zst"
                        document["corresponding_sources"] = [{
                            "name": "ragel", "archive_file": name,
                            "archive_url": "https://repo.msys2.org/mingw/sources/" + name,
                            "archive_sha256": real_sha256(b"123456789").hexdigest(),
                            "license_obligation": "synthetic source fixture",
                        }]
                    self.lock.write_text(json.dumps(document))
                    if cached:
                        (cache / name).write_bytes(b"123456789")
                    def download(url, target, *, max_bytes):
                        self.assertEqual(max_bytes, 8)
                        target.write_bytes(b"123456789")
                    prefix_hashers = [real_sha256()] if source else []
                    with mock.patch.object(tools, "MAX_PACKAGE_SIZE", 1024 if source else 8), mock.patch.object(
                        tools, "MAX_SOURCE_SIZE", 8
                    ), mock.patch.object(tools.hashlib, "sha256", side_effect=prefix_hashers + [
                        AssertionError("hashed oversized MSYS2 archive")
                    ]) as hashed:
                        with self.assertRaisesRegex(RuntimeError, "exceeds"):
                            tools.materialize(cache, self.destination, self.lock, downloader=download,
                                              decompressor=lambda archive: io.BytesIO(self.payload))
                    self.assertEqual(hashed.call_count, 1 if source else 0)
                    self.assertFalse(self.destination.exists())
                    self.assertEqual((cache / name).exists(), cached)
                    if cached:
                        self.assertEqual((cache / name).read_bytes(), b"123456789")

    def test_existing_artifact_is_never_overwritten(self):
        self.destination.write_bytes(b"existing artifact")
        with self.assertRaisesRegex(RuntimeError, "already exists"):
            self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        self.assertEqual(self.destination.read_bytes(), b"existing artifact")

    def test_transient_download_error_retries_without_partial_cache(self):
        attempts = []
        def downloader(url, target, *, max_bytes):
            attempts.append(url)
            if len(attempts) < 3:
                target.write_bytes(b"partial")
                raise OSError("transient network error")
            target.write_bytes(self.data)
        with mock.patch.object(tools.time, "sleep"):
            self._materialize(downloader=downloader)
        self.assertEqual(len(attempts), 3)
        self.assertEqual((self.cache / self.filename).read_bytes(), self.data)

    def test_permanent_http_failure_is_not_retried(self):
        response = io.BytesIO(b"missing")
        failed = mock.Mock(side_effect=urllib.error.HTTPError("https://repo.msys2.org/", 404,
                                                             "missing", {}, response))
        with mock.patch.object(tools.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "download"):
                self._materialize(downloader=failed)
        failed.assert_called_once()
        self.assertTrue(response.closed)
        self.assertFalse((self.cache / self.filename).exists())

    def test_exhausted_download_retries_leave_no_partial_cache(self):
        with mock.patch.object(tools.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "download"):
                self._materialize(downloader=mock.Mock(side_effect=OSError("offline")))
        self.assertFalse((self.cache / self.filename).exists())
        self.assertFalse(self.destination.exists())

    def test_corresponding_source_must_be_hashed_and_transported_with_artifact(self):
        source = b"upstream Ragel tarball and MSYS2 PKGBUILD"
        source_name = "mingw-w64-ragel-6.10-3.src.tar.zst"
        self.document["corresponding_sources"] = [{
            "name": "ragel", "archive_file": source_name,
            "archive_url": "https://repo.msys2.org/mingw/sources/" + source_name,
            "archive_sha256": hashlib.sha256(source).hexdigest(),
            "license_obligation": "GPLv3 corresponding source and build scripts",
        }]
        self.lock.write_text(json.dumps(self.document))
        def downloader(url, target, *, max_bytes):
            target.write_bytes(source if url.endswith(source_name) else self.data)
        self._materialize(downloader=downloader)
        with zipfile.ZipFile(self.destination) as archive:
            self.assertEqual(archive.read("corresponding-source/" + source_name), source)
        installed = self.root / "installed"
        tools.extract_artifact(self.destination, installed, self.lock)
        self.assertFalse((installed / "corresponding-source").exists())
        with zipfile.ZipFile(self.destination) as archive:
            content = {name: archive.read(name) for name in archive.namelist()}
        content["corresponding-source/" + source_name] = b"tampered"
        with zipfile.ZipFile(self.destination, "w") as archive:
            for name, data in content.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            tools.extract_artifact(self.destination, self.root / "tampered", self.lock)

    def test_cached_archive_is_rehashed_before_extraction(self):
        self.cache.mkdir()
        (self.cache / self.filename).write_bytes(b"corrupted")
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            self._materialize(downloader=mock.Mock())
        self.assertFalse(self.destination.exists())

    def test_verified_download_and_artifact_roundtrip(self):
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        with zipfile.ZipFile(self.destination) as zip_file:
            self.assertEqual(zip_file.read("msys64/mingw64/bin/ragel.exe"), b"ragel binary")
            self.assertNotIn("msys64/mingw64/bin/gcc.exe", zip_file.namelist())
        installed = self.root / "installed"
        tools.extract_artifact(self.destination, installed, self.lock)
        self.assertEqual((installed / "msys64/mingw64/bin/ragel.exe").read_bytes(), b"ragel binary")

    def test_path_traversal_is_rejected_after_archive_digest_matches(self):
        self.members["../escape"] = b"malicious"
        self._make_package()
        with self.assertRaisesRegex(RuntimeError, "path"):
            self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        self.assertFalse((self.root / "escape").exists())
        self.assertFalse(self.destination.exists())

    def test_license_directories_in_official_archives_are_allowed(self):
        self.members["mingw64/share/licenses/ragel/"] = b""
        self._make_package()
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        self.assertTrue(self.destination.is_file())

    def test_timezone_relative_symlinks_are_materialized_as_bytes(self):
        self.document["packages"][0]["runtime_prefixes"] = ["mingw64/share/zoneinfo/"]
        self.members["mingw64/share/zoneinfo/Etc/UTC"] = b"tz bytes"
        self.lock.write_text(json.dumps(self.document))
        contents = io.BytesIO()
        with tarfile.open(fileobj=contents, mode="w") as tar:
            for name, data in self.members.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
            link = tarfile.TarInfo("mingw64/share/zoneinfo/UTC")
            link.type = tarfile.SYMTYPE
            link.linkname = "Etc/UTC"
            tar.addfile(link)
            hardlink = tarfile.TarInfo("mingw64/share/zoneinfo/GMT")
            hardlink.type = tarfile.LNKTYPE
            hardlink.linkname = "mingw64/share/zoneinfo/Etc/UTC"
            tar.addfile(hardlink)
        self.payload = contents.getvalue()
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        with zipfile.ZipFile(self.destination) as output:
            self.assertEqual(output.read("msys64/mingw64/share/zoneinfo/UTC"), b"tz bytes")

    def test_timezone_symlink_cannot_escape_selected_runtime_tree(self):
        self.document["packages"][0]["runtime_prefixes"] = ["mingw64/share/zoneinfo/"]
        self.lock.write_text(json.dumps(self.document))
        contents = io.BytesIO()
        with tarfile.open(fileobj=contents, mode="w") as tar:
            for name, data in self.members.items():
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                tar.addfile(entry, io.BytesIO(data))
            link = tarfile.TarInfo("mingw64/share/zoneinfo/UTC")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../../../escape"
            tar.addfile(link)
        self.payload = contents.getvalue()
        with self.assertRaisesRegex(RuntimeError, "path"):
            self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        self.assertFalse(self.destination.exists())

    def test_package_version_is_checked_even_for_hash_verified_archive(self):
        self.members[".PKGINFO"] = f"pkgname = {self.package}\npkgver = 6.10-2\n".encode()
        self._make_package()
        with self.assertRaisesRegex(RuntimeError, "version"):
            self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        self.assertFalse(self.destination.exists())

    def test_download_sha_failure_does_not_poison_cache(self):
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(b"invalid"))
        self.assertFalse((self.cache / self.filename).exists())

    def test_unlisted_compiler_executable_is_not_redistributed(self):
        self.members["mingw64/bin/gcc.exe"] = b"compiler"
        self._make_package()
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        with zipfile.ZipFile(self.destination) as zip_file:
            self.assertNotIn("msys64/mingw64/bin/gcc.exe", zip_file.namelist())

    def test_tampered_artifact_and_path_traversal_fail_closed(self):
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        with zipfile.ZipFile(self.destination) as original:
            content = {name: original.read(name) for name in original.namelist()}
        content["msys64/mingw64/bin/ragel.exe"] = b"tampered"
        with zipfile.ZipFile(self.destination, "w") as archive:
            for name, data in content.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            tools.extract_artifact(self.destination, self.root / "installed", self.lock)
        self.assertFalse((self.root / "installed").exists())
        content["msys64/mingw64/bin/ragel.exe"] = b"ragel binary"
        content["../escape"] = b"malicious"
        with zipfile.ZipFile(self.destination, "w") as archive:
            for name, data in content.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(RuntimeError, "path"):
            tools.extract_artifact(self.destination, self.root / "installed", self.lock)

    def test_zip_symlink_entries_are_rejected_before_install(self):
        self._materialize(downloader=lambda url, target, *, max_bytes: target.write_bytes(self.data))
        with zipfile.ZipFile(self.destination) as original:
            content = {name: original.read(name) for name in original.namelist()}
        with zipfile.ZipFile(self.destination, "w") as archive:
            for name, data in content.items():
                if name == "msys64/mingw64/bin/ragel.exe":
                    member = zipfile.ZipInfo(name)
                    member.create_system = 3
                    member.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(member, data)
                else:
                    archive.writestr(name, data)
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            tools.extract_artifact(self.destination, self.root / "installed", self.lock)
        self.assertFalse((self.root / "installed").exists())

    def test_official_lock_has_exact_nine_packages_and_runtime_edges(self):
        lock = json.loads((ROOT / "ci/tools/msys2-tools.json").read_text())
        packages = {package["name"]: package for package in lock["packages"]}
        self.assertEqual(set(packages), {"ragel", "pkgconf", "cc-libs", "libatomic", "libgcc", "libquadmath", "libstdc++", "libwinpthread", "tzdata"})
        expected = {
            "ragel": ("6.10-3", "8b12c4c999ccfa7082d6b0cd7ef167d6ac98708923c8c0eff917dbb7070e8773"),
            "pkgconf": ("1~3.0.7-1", "37b97f372409fa7cf54d709bab892f34e9c62d2658230a09f783a89a55ae590e"),
            "cc-libs": ("16.2.0-4", "691c243e8df80ab075cabf79236b9e2281907fb1f3ac7206fe3a2d71269e4fa2"),
            "libatomic": ("16.2.0-4", "c71710695b1a49a089e82c4fc4e2247003bfb7ac8ef3327291880eaa59dcebdc"),
            "libgcc": ("16.2.0-4", "d615f6a8536ca16b1f049daea1fa440a3b7a405449ec0e93ad6106db43682d54"),
            "libquadmath": ("16.2.0-4", "70317ed08299f8e40af8354e124db33fcf47840ec8cefbea5d7134c6d136d13e"),
            "libstdc++": ("16.2.0-4", "3d4c3faf4c2c5c7a851ff12214ddcbf8c0d6df0964fc1d3ebd8c38f227183034"),
            "libwinpthread": ("14.0.0.r426.g4564ee4b5-1", "543017ce2731292b215bf1d36fd70a86d8a8d5ed0afba9d3db9fff89804cda71"),
            "tzdata": ("2026d-1", "aa05089a14869bc05f841ce1caa61bb4ebbc392780f9daec4f3eaf01937e5091"),
        }
        self.assertEqual({name: (package["version"], package["archive_sha256"])
                          for name, package in packages.items()}, expected)
        self.assertEqual(packages["libstdc++"]["depends"], ["libgcc", "libwinpthread", "tzdata"])
        self.assertIn("GPL-2.0-or-later", packages["ragel"]["license"])
        self.assertIn("GPL-2.0-or-later", next(
            source["license_obligation"] for source in lock["corresponding_sources"]
            if source["name"] == "ragel"
        ))
        self.assertEqual({source["name"]: source["archive_sha256"]
                          for source in lock["corresponding_sources"]}, {
            "ragel": "af0cb703bb7ccd77b3cf75dd91d4dce0452c0b96ae6b3d678ae40386184ec4b6",
            "gcc": "a1792a44764405bc365746e0e60f0daa122076cec0dddd35a781d141f94273cf",
        })
        tools.load_lock(ROOT / "ci/tools/msys2-tools.json")

    def test_workflow_event_projections_have_one_pinned_producer_and_three_verified_consumers(self):
        text = (ROOT / ".github/workflows/ci-build.yml").read_text()
        producer = text.split("  PrefetchWindowsTools:", 1)[1].split("  PrefetchAdbHelperSources:", 1)[0]
        consumers = text.split("  WindowsPackages:", 1)[1].split("  ci-gate:", 1)[0]
        self.assertIn("uses: actions/checkout@", producer)
        self.assertIn("hashFiles('ci/tools/msys2-tools.json')", producer)
        self.assertIn("python3 scripts/materialize_windows_tools.py", producer)
        self.assertNotIn("msys2/setup-msys2", producer)
        self.assertNotIn("pacman", producer)
        self.assertIn("--require-prefetched", consumers)
        self.assertIn("--extract-artifact", consumers)
        self.assertNotIn("C:\\msys64", consumers)
        gate = "(github.event_name != 'workflow_dispatch' || (inputs.environment-mode == 'off' && inputs.dependency-mode == 'off')) && !contains(github.event.head_commit.message, '[skip ci]')"
        self.assertEqual(producer.count("if: ${{ " + gate + " }}"), 1)
        self.assertEqual(consumers.count("if: ${{ " + gate + " }}"), 3)
        for event, environment_mode, dependency_mode, expected in (
                ("pull_request", "off", "off", True),
                ("push", "off", "off", True),
                ("workflow_dispatch", "off", "off", True),
                ("workflow_dispatch", "qualify", "off", False),
                ("workflow_dispatch", "off", "qualify", False),
                ("workflow_dispatch", "off", "publish", False)):
            self.assertEqual(event != "workflow_dispatch" or (
                environment_mode == "off" and dependency_mode == "off"
            ), expected, (event, environment_mode, dependency_mode))
        self.assertIn("steps: *windows_steps", consumers)
        self.assertEqual(consumers.count("steps: *windows_steps"), 2)


if __name__ == "__main__":
    unittest.main()
