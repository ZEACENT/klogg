import hashlib
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import materialize_boost as boost


SOURCE_ROOT = "boost_1_86_0"
VERSION = b'#define BOOST_VERSION 108600\n'


def archive_bytes(version=VERSION, extra=()):
    payload = io.BytesIO()
    with tarfile.open(fileobj=payload, mode="w:bz2") as archive:
        for name, content in ((f"{SOURCE_ROOT}/boost/version.hpp", version), *extra):
            entry = tarfile.TarInfo(name)
            entry.size = len(content)
            archive.addfile(entry, io.BytesIO(content))
    return payload.getvalue()


class MaterializeBoostTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.archive = self.root / "boost.tar.bz2"
        self.destination = self.root / "boost"
        self.manifest = self.root / "materials.json"
        self.valid = archive_bytes()
        self.manifest.write_text(json.dumps({"schema_version": 1, "assets": {
            "boost1860": {"url": "https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2",
                          "sha256": hashlib.sha256(self.valid).hexdigest()}
        }}))

    def run_materialize(self, *, require_prefetched=False, verify_only=False, downloader=None):
        return boost.materialize(self.archive, self.destination, self.manifest,
                                 require_prefetched=require_prefetched,
                                 verify_only=verify_only, downloader=downloader)

    def test_cached_archive_is_hashed_even_if_version_header_is_already_present(self):
        self.archive.write_bytes(b"untrusted cached bytes")
        (self.destination / "boost").mkdir(parents=True)
        (self.destination / "boost/version.hpp").write_bytes(VERSION)
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            self.run_materialize(downloader=mock.Mock())
        self.assertEqual((self.destination / "boost/version.hpp").read_bytes(), VERSION)

    def test_prefetched_archive_is_required_even_if_tree_exists(self):
        (self.destination / "boost").mkdir(parents=True)
        (self.destination / "boost/version.hpp").write_bytes(VERSION)
        with self.assertRaisesRegex(RuntimeError, "prefetched Boost archive"):
            self.run_materialize(require_prefetched=True, downloader=mock.Mock())

    def test_source_version_must_match_before_publishing_tree(self):
        wrong = archive_bytes(b'#define BOOST_VERSION 108500\n')
        self.archive.write_bytes(wrong)
        self.manifest.write_text(json.dumps({"schema_version": 1, "assets": {
            "boost1860": {"url": "https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2",
                          "sha256": hashlib.sha256(wrong).hexdigest()}
        }}))
        with self.assertRaisesRegex(RuntimeError, "Boost source version"):
            self.run_materialize()
        self.assertFalse(self.destination.exists())

    def test_valid_prefetched_archive_materializes_exact_tree(self):
        self.archive.write_bytes(self.valid)
        self.run_materialize(require_prefetched=True)
        self.assertEqual((self.destination / "boost/version.hpp").read_bytes(), VERSION)

    def test_direct_acquisition_uses_manifest_url_then_checks_bytes(self):
        calls = []
        def download(url, destination):
            calls.append(url)
            destination.write_bytes(self.valid)
        self.run_materialize(downloader=download)
        self.assertEqual(calls, ["https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2"])
        self.assertEqual((self.destination / "boost/version.hpp").read_bytes(), VERSION)
        self.run_materialize(downloader=mock.Mock(side_effect=AssertionError("cache hit downloaded")))

    def test_mismatched_download_is_never_cached_or_extracted(self):
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            self.run_materialize(downloader=lambda url, path: path.write_bytes(b"invalid"))
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.destination.exists())

    def test_verify_only_does_not_extract(self):
        self.archive.write_bytes(self.valid)
        self.run_materialize(verify_only=True, require_prefetched=True)
        self.assertFalse(self.destination.exists())

    def test_workflow_prefetch_transports_only_verified_original_archive(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text()
        prefetch = workflow.split("  PrefetchBoost:", 1)[1].split("  PrefetchOpenSsl:", 1)[0]
        self.assertIn("uses: actions/checkout@", prefetch)
        self.assertIn("python3 scripts/materialize_boost.py", prefetch)
        self.assertIn("hashFiles('ci/environments/materials.json')", prefetch)
        self.assertNotIn("cache-hit", prefetch)
        self.assertNotIn("sourceforge.net", prefetch)
        self.assertNotIn("tar -czf", prefetch)
        self.assertIn("path: prefetch_artifacts/boost_1_86_0.tar.bz2", prefetch)

    def test_native_consumers_verify_prefetched_archive_before_extraction(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text()
        self.assertEqual(workflow.count("name: boost-root"), 3)
        self.assertNotIn("tar -xzf $boostArchive", workflow)
        self.assertNotIn('tar -xzf "${{ github.workspace }}/boost.tar.gz"', workflow)
        setup = (ROOT / ".github/actions/agent-setup/action.yml").read_text()
        self.assertIn("python3 scripts/materialize_boost.py", setup)
        self.assertIn("--require-prefetched", setup)
        self.assertIn("hashFiles('ci/environments/materials.json')", setup)
        self.assertNotIn("sourceforge.net", setup)
        self.assertNotIn("boost/version.hpp", setup)

    def test_path_traversal_is_rejected_after_hash_verification(self):
        bad = archive_bytes(extra=(("../escape", b"payload"),))
        self.archive.write_bytes(bad)
        self.manifest.write_text(json.dumps({"schema_version": 1, "assets": {
            "boost1860": {"url": "https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2",
                          "sha256": hashlib.sha256(bad).hexdigest()}
        }}))
        with self.assertRaises(RuntimeError):
            self.run_materialize()
        self.assertFalse((self.root / "escape").exists())
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main()
